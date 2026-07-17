#include "endstone_exchange/database.hpp"

#include <errmsg.h>
#include <mysql.h>

#include <array>
#include <format>
#include <limits>
#include <sstream>
#include <utility>

namespace exchange {
namespace {

// MySQL 8.0.24+ reports wait_timeout disconnects with this server error
// instead of the older client-side CR_SERVER_GONE_ERROR in some connectors.
constexpr unsigned int MySqlClientInteractionTimeout = 4031;

bool isDisconnectError(const unsigned int error_code) {
    return error_code == CR_SERVER_GONE_ERROR || error_code == CR_SERVER_LOST ||
           error_code == MySqlClientInteractionTimeout;
}

} // namespace

struct Database::Impl {
    MYSQL *connection{nullptr};
    bool in_transaction{false};
    bool reconnecting{false};

    ~Impl() {
        if (connection != nullptr) {
            mysql_close(connection);
        }
    }
};

Database::Database(DatabaseConfig config) : config_(std::move(config)), impl_(std::make_unique<Impl>()) {}

Database::~Database() = default;
Database::Database(Database &&) noexcept = default;
Database &Database::operator=(Database &&) noexcept = default;

void Database::connect() {
    if (impl_->in_transaction) {
        throw DatabaseError("cannot reconnect during a database transaction");
    }
    if (impl_->connection != nullptr) {
        mysql_close(impl_->connection);
        impl_->connection = nullptr;
    }

    MYSQL *connection = mysql_init(nullptr);
    if (connection == nullptr) {
        throw DatabaseError("mysql_init failed");
    }
    impl_->connection = connection;
    impl_->in_transaction = false;
    mysql_options(connection, MYSQL_OPT_CONNECT_TIMEOUT, &config_.connect_timeout_seconds);
    mysql_options(connection, MYSQL_OPT_READ_TIMEOUT, &config_.connect_timeout_seconds);
    mysql_options(connection, MYSQL_OPT_WRITE_TIMEOUT, &config_.connect_timeout_seconds);
    mysql_options(connection, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    if (mysql_real_connect(connection, config_.host.c_str(), config_.user.c_str(), config_.password.c_str(), nullptr,
                           config_.port, nullptr, CLIENT_FOUND_ROWS) == nullptr) {
        const std::string message = mysql_error(connection);
        throw DatabaseError("database connection failed: " + message);
    }

    execute(std::format("CREATE DATABASE IF NOT EXISTS `{}` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci",
                        config_.name));
    if (mysql_select_db(connection, config_.name.c_str()) != 0) {
        throw DatabaseError("cannot select database: " + std::string(mysql_error(connection)));
    }
    execute("SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED");
    execute("SET SESSION sql_mode = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION'");
}

void Database::migrate() {
    static constexpr std::array<std::string_view, 7> version_1 = {
        R"sql(CREATE TABLE IF NOT EXISTS exchange_schema_versions (
            version INT NOT NULL PRIMARY KEY,
            applied_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)sql",
        R"sql(CREATE TABLE IF NOT EXISTS exchange_accounts (
            player_uuid CHAR(36) NOT NULL PRIMARY KEY,
            player_name VARCHAR(64) NOT NULL,
            balance_cents BIGINT NOT NULL,
            created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
            updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
            CHECK (balance_cents >= 0)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)sql",
        R"sql(CREATE TABLE IF NOT EXISTS exchange_markets (
            id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
            target_key VARCHAR(255) NOT NULL UNIQUE,
            target_kind ENUM('BLOCK', 'ACTOR') NOT NULL,
            dimension_name VARCHAR(96) NOT NULL,
            block_x INT NULL,
            block_y INT NULL,
            block_z INT NULL,
            actor_id BIGINT NULL,
            item_type VARCHAR(160) NOT NULL,
            item_data INT NOT NULL DEFAULT 0,
            item_nbt LONGBLOB NOT NULL,
            item_name VARCHAR(255) NOT NULL,
            active BOOLEAN NOT NULL DEFAULT TRUE,
            created_by CHAR(36) NOT NULL,
            created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
            updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
            INDEX idx_exchange_markets_active (active)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)sql",
        R"sql(CREATE TABLE IF NOT EXISTS exchange_orders (
            id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
            market_id BIGINT UNSIGNED NOT NULL,
            player_uuid CHAR(36) NOT NULL,
            side ENUM('BUY', 'SELL') NOT NULL,
            order_type ENUM('LIMIT', 'MARKET') NOT NULL,
            price_cents BIGINT NOT NULL,
            original_qty INT NOT NULL,
            remaining_qty INT NOT NULL,
            reserved_cents BIGINT NOT NULL DEFAULT 0,
            status ENUM('OPEN', 'PARTIAL', 'FILLED', 'CANCELED') NOT NULL,
            created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
            updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
            INDEX idx_exchange_orders_book (market_id, side, order_type, status, price_cents, id),
            INDEX idx_exchange_orders_owner (player_uuid, status, id),
            CONSTRAINT fk_exchange_orders_market FOREIGN KEY (market_id) REFERENCES exchange_markets(id),
            CONSTRAINT fk_exchange_orders_account FOREIGN KEY (player_uuid) REFERENCES exchange_accounts(player_uuid),
            CHECK (price_cents >= 0),
            CHECK (original_qty > 0),
            CHECK (remaining_qty >= 0),
            CHECK (reserved_cents >= 0)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)sql",
        R"sql(CREATE TABLE IF NOT EXISTS exchange_trades (
            id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
            market_id BIGINT UNSIGNED NOT NULL,
            buy_order_id BIGINT UNSIGNED NOT NULL,
            sell_order_id BIGINT UNSIGNED NOT NULL,
            buyer_uuid CHAR(36) NOT NULL,
            seller_uuid CHAR(36) NOT NULL,
            price_cents BIGINT NOT NULL,
            quantity INT NOT NULL,
            created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
            INDEX idx_exchange_trades_market (market_id, id),
            CONSTRAINT fk_exchange_trades_market FOREIGN KEY (market_id) REFERENCES exchange_markets(id),
            CHECK (price_cents > 0),
            CHECK (quantity > 0)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)sql",
        R"sql(CREATE TABLE IF NOT EXISTS exchange_deliveries (
            id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
            player_uuid CHAR(36) NOT NULL,
            market_id BIGINT UNSIGNED NOT NULL,
            quantity INT NOT NULL,
            claimed_qty INT NOT NULL DEFAULT 0,
            reason VARCHAR(32) NOT NULL,
            created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
            updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
            INDEX idx_exchange_deliveries_pending (player_uuid, claimed_qty, quantity, id),
            CONSTRAINT fk_exchange_deliveries_market FOREIGN KEY (market_id) REFERENCES exchange_markets(id),
            CHECK (quantity > 0),
            CHECK (claimed_qty >= 0 AND claimed_qty <= quantity)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)sql",
        "INSERT IGNORE INTO exchange_schema_versions(version) VALUES (1)",
    };

    for (const auto statement : version_1) {
        execute(statement);
    }

    const auto version_2_applied = query("SELECT version FROM exchange_schema_versions WHERE version=2");
    if (version_2_applied.empty()) {
        // A target may be reopened later with a different item. Keep every market
        // row immutable so historical orders, trades, and deliveries retain the
        // exact item snapshot they were created for. This binding table is the
        // single mutable pointer from a world target to its current market.
        execute(R"sql(CREATE TABLE IF NOT EXISTS exchange_target_bindings (
            target_key VARCHAR(255) NOT NULL PRIMARY KEY,
            market_id BIGINT UNSIGNED NOT NULL UNIQUE,
            bound_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
            CONSTRAINT fk_exchange_target_bindings_market
                FOREIGN KEY (market_id) REFERENCES exchange_markets(id)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)sql");
        execute(R"sql(INSERT INTO exchange_target_bindings(target_key, market_id)
            SELECT target_key, id FROM exchange_markets WHERE active=1
            ON DUPLICATE KEY UPDATE market_id=VALUES(market_id))sql");

        const auto legacy_unique = query(R"sql(SELECT INDEX_NAME FROM INFORMATION_SCHEMA.STATISTICS
            WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='exchange_markets' AND INDEX_NAME='target_key' LIMIT 1)sql");
        if (!legacy_unique.empty()) {
            execute("ALTER TABLE exchange_markets DROP INDEX target_key");
        }
        const auto history_index = query(R"sql(SELECT INDEX_NAME FROM INFORMATION_SCHEMA.STATISTICS
            WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='exchange_markets'
              AND INDEX_NAME='idx_exchange_markets_target_history' LIMIT 1)sql");
        if (history_index.empty()) {
            execute("CREATE INDEX idx_exchange_markets_target_history ON exchange_markets(target_key,id)");
        }
        execute("INSERT INTO exchange_schema_versions(version) VALUES (2)");
    }

    const auto version_3_applied = query("SELECT version FROM exchange_schema_versions WHERE version=3");
    if (version_3_applied.empty()) {
        const auto reserved_column = query(R"sql(SELECT COLUMN_NAME FROM INFORMATION_SCHEMA.COLUMNS
            WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='exchange_deliveries'
              AND COLUMN_NAME='reserved_qty' LIMIT 1)sql");
        if (reserved_column.empty()) {
            execute("ALTER TABLE exchange_deliveries ADD COLUMN reserved_qty INT NOT NULL DEFAULT 0 AFTER claimed_qty");
        }
        const auto delivery_claim_check = query(R"sql(SELECT CONSTRAINT_NAME FROM INFORMATION_SCHEMA.TABLE_CONSTRAINTS
            WHERE CONSTRAINT_SCHEMA=DATABASE() AND TABLE_NAME='exchange_deliveries'
              AND CONSTRAINT_NAME='chk_exchange_deliveries_claims' LIMIT 1)sql");
        if (delivery_claim_check.empty()) {
            execute(R"sql(ALTER TABLE exchange_deliveries ADD CONSTRAINT chk_exchange_deliveries_claims
                CHECK (claimed_qty >= 0 AND reserved_qty >= 0 AND claimed_qty + reserved_qty <= quantity))sql");
        }
        execute(R"sql(CREATE TABLE IF NOT EXISTS exchange_delivery_claims (
            id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
            delivery_id BIGINT UNSIGNED NOT NULL,
            player_uuid CHAR(36) NOT NULL,
            quantity INT NOT NULL,
            applied_qty INT NOT NULL DEFAULT 0,
            status ENUM('PREPARED','APPLIED','CANCELED') NOT NULL DEFAULT 'PREPARED',
            created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
            applied_at TIMESTAMP(6) NULL,
            cleaned_at TIMESTAMP(6) NULL,
            INDEX idx_exchange_delivery_claims_unfinished (player_uuid,status,cleaned_at,id),
            CONSTRAINT fk_exchange_delivery_claims_delivery
                FOREIGN KEY (delivery_id) REFERENCES exchange_deliveries(id),
            CHECK (quantity > 0),
            CHECK (applied_qty >= 0 AND applied_qty <= quantity)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)sql");
        execute("INSERT INTO exchange_schema_versions(version) VALUES (3)");
    }

    const auto version_4_applied = query("SELECT version FROM exchange_schema_versions WHERE version=4");
    if (version_4_applied.empty()) {
        execute(R"sql(CREATE TABLE IF NOT EXISTS exchange_sell_escrows (
            id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
            market_id BIGINT UNSIGNED NOT NULL,
            player_uuid CHAR(36) NOT NULL,
            player_name VARCHAR(64) NOT NULL,
            order_type ENUM('LIMIT','MARKET') NOT NULL,
            price_cents BIGINT NOT NULL,
            requested_qty INT NOT NULL,
            tagged_qty INT NOT NULL DEFAULT 0,
            receipt_count INT NOT NULL DEFAULT 0,
            status ENUM('PREPARED','TAGGED','ORDERED','CANCELED') NOT NULL DEFAULT 'PREPARED',
            order_id BIGINT UNSIGNED NULL UNIQUE,
            created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
            tagged_at TIMESTAMP(6) NULL,
            ordered_at TIMESTAMP(6) NULL,
            cleaned_at TIMESTAMP(6) NULL,
            INDEX idx_exchange_sell_escrows_unfinished (player_uuid,status,cleaned_at,id),
            CONSTRAINT fk_exchange_sell_escrows_market FOREIGN KEY (market_id) REFERENCES exchange_markets(id),
            CONSTRAINT fk_exchange_sell_escrows_account FOREIGN KEY (player_uuid) REFERENCES exchange_accounts(player_uuid),
            CONSTRAINT fk_exchange_sell_escrows_order FOREIGN KEY (order_id) REFERENCES exchange_orders(id),
            CHECK (price_cents >= 0),
            CHECK (requested_qty > 0),
            CHECK (tagged_qty = 0 OR tagged_qty >= requested_qty),
            CHECK (receipt_count >= 0)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)sql");
        execute("INSERT INTO exchange_schema_versions(version) VALUES (4)");
    }

    const auto version_5_applied = query("SELECT version FROM exchange_schema_versions WHERE version=5");
    if (version_5_applied.empty()) {
        execute(R"sql(CREATE TABLE IF NOT EXISTS exchange_balance_ledger (
            id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
            player_uuid CHAR(36) NOT NULL,
            delta_cents BIGINT NOT NULL,
            reason VARCHAR(48) NOT NULL,
            reference_type VARCHAR(24) NULL,
            reference_id BIGINT UNSIGNED NULL,
            created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
            INDEX idx_exchange_balance_ledger_player (player_uuid,id),
            INDEX idx_exchange_balance_ledger_reference (reference_type,reference_id),
            CONSTRAINT fk_exchange_balance_ledger_account
                FOREIGN KEY (player_uuid) REFERENCES exchange_accounts(player_uuid)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)sql");
        execute(R"sql(INSERT INTO exchange_balance_ledger(player_uuid,delta_cents,reason)
            SELECT a.player_uuid,a.balance_cents,'MIGRATION_OPENING_BALANCE' FROM exchange_accounts a
            WHERE NOT EXISTS (SELECT 1 FROM exchange_balance_ledger l WHERE l.player_uuid=a.player_uuid))sql");
        execute("INSERT INTO exchange_schema_versions(version) VALUES (5)");
    }

    const auto version_6_applied = query("SELECT version FROM exchange_schema_versions WHERE version=6");
    if (!version_6_applied.empty()) {
        return;
    }
    const auto reopenable_column = query(R"sql(SELECT COLUMN_NAME FROM INFORMATION_SCHEMA.COLUMNS
        WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='exchange_markets'
          AND COLUMN_NAME='reopenable' LIMIT 1)sql");
    if (reopenable_column.empty()) {
        execute("ALTER TABLE exchange_markets ADD COLUMN reopenable BOOLEAN NOT NULL DEFAULT FALSE AFTER active");
    }
    execute("INSERT INTO exchange_schema_versions(version) VALUES (6)");
}

void Database::ping() {
    ensureConnected();
    if (mysql_ping(impl_->connection) != 0) {
        const auto error_code = mysql_errno(impl_->connection);
        const auto message = std::string(mysql_error(impl_->connection));
        if (!impl_->in_transaction && !impl_->reconnecting && isDisconnectError(error_code)) {
            impl_->reconnecting = true;
            try {
                connect();
                impl_->reconnecting = false;
                return;
            } catch (...) {
                impl_->reconnecting = false;
                throw;
            }
        }
        throw DatabaseError("database ping failed: " + message);
    }
}

void Database::execute(const std::string_view sql) {
    ensureConnected();
    if (mysql_real_query(impl_->connection, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
        const auto error_code = mysql_errno(impl_->connection);
        if (!impl_->in_transaction && !impl_->reconnecting && isDisconnectError(error_code)) {
            impl_->reconnecting = true;
            try {
                connect();
                impl_->reconnecting = false;
            } catch (...) {
                impl_->reconnecting = false;
                throw;
            }
            if (mysql_real_query(impl_->connection, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
                throw DatabaseError("database statement failed after reconnect: " +
                                    std::string(mysql_error(impl_->connection)));
            }
        } else {
            throw DatabaseError("database statement failed: " + std::string(mysql_error(impl_->connection)));
        }
    }
    if (MYSQL_RES *result = mysql_store_result(impl_->connection); result != nullptr) {
        mysql_free_result(result);
    } else if (mysql_field_count(impl_->connection) != 0) {
        throw DatabaseError("database result failed: " + std::string(mysql_error(impl_->connection)));
    }
}

QueryRows Database::query(const std::string_view sql) {
    ensureConnected();
    if (mysql_real_query(impl_->connection, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
        const auto error_code = mysql_errno(impl_->connection);
        if (!impl_->in_transaction && !impl_->reconnecting && isDisconnectError(error_code)) {
            impl_->reconnecting = true;
            try {
                connect();
                impl_->reconnecting = false;
            } catch (...) {
                impl_->reconnecting = false;
                throw;
            }
            if (mysql_real_query(impl_->connection, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
                throw DatabaseError("database query failed after reconnect: " +
                                    std::string(mysql_error(impl_->connection)));
            }
        } else {
            throw DatabaseError("database query failed: " + std::string(mysql_error(impl_->connection)));
        }
    }
    MYSQL_RES *result = mysql_store_result(impl_->connection);
    if (result == nullptr) {
        if (mysql_field_count(impl_->connection) == 0) {
            return {};
        }
        throw DatabaseError("database result failed: " + std::string(mysql_error(impl_->connection)));
    }

    QueryRows rows;
    const auto fields = mysql_num_fields(result);
    while (MYSQL_ROW raw = mysql_fetch_row(result)) {
        const unsigned long *lengths = mysql_fetch_lengths(result);
        QueryRow row;
        row.reserve(fields);
        for (unsigned int index = 0; index < fields; ++index) {
            if (raw[index] == nullptr) {
                row.emplace_back(std::nullopt);
            } else {
                row.emplace_back(std::string(raw[index], lengths[index]));
            }
        }
        rows.push_back(std::move(row));
    }
    mysql_free_result(result);
    return rows;
}

std::uint64_t Database::lastInsertId() const {
    ensureConnected();
    return mysql_insert_id(impl_->connection);
}

std::uint64_t Database::affectedRows() const {
    ensureConnected();
    const auto rows = mysql_affected_rows(impl_->connection);
    if (rows == std::numeric_limits<my_ulonglong>::max()) {
        throw DatabaseError("cannot read affected row count: " + std::string(mysql_error(impl_->connection)));
    }
    return rows;
}

void Database::begin() {
    if (impl_->in_transaction) {
        throw DatabaseError("database transaction is already active");
    }
    execute("START TRANSACTION");
    impl_->in_transaction = true;
}

void Database::commit() {
    execute("COMMIT");
    impl_->in_transaction = false;
}

void Database::rollback() noexcept {
    if (impl_ != nullptr && impl_->connection != nullptr) {
        mysql_rollback(impl_->connection);
        impl_->in_transaction = false;
    }
}

std::string Database::quote(const std::string_view value) const {
    ensureConnected();
    std::string escaped(value.size() * 2 + 1, '\0');
    const auto length = mysql_real_escape_string(impl_->connection, escaped.data(), value.data(),
                                                 static_cast<unsigned long>(value.size()));
    escaped.resize(length);
    return "'" + escaped + "'";
}

std::string Database::hexLiteral(const std::vector<std::uint8_t> &value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(3 + value.size() * 2);
    result += "X'";
    for (const auto byte : value) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    result.push_back('\'');
    return result;
}

void Database::ensureConnected() const {
    if (impl_ == nullptr || impl_->connection == nullptr) {
        throw DatabaseError("database is not connected");
    }
}

Transaction::Transaction(Database &database) : database_(database) {
    database_.begin();
}

Transaction::~Transaction() {
    if (!committed_) {
        database_.rollback();
    }
}

void Transaction::commit() {
    database_.commit();
    committed_ = true;
}

std::int64_t cellInt64(const QueryRow &row, const std::size_t index) {
    const auto value = cellString(row, index);
    std::size_t consumed = 0;
    const auto result = std::stoll(value, &consumed);
    if (consumed != value.size()) {
        throw DatabaseError("database returned a non-integer value");
    }
    return result;
}

int cellInt(const QueryRow &row, const std::size_t index) {
    const auto value = cellInt64(row, index);
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        throw DatabaseError("database integer is outside int range");
    }
    return static_cast<int>(value);
}

std::string cellString(const QueryRow &row, const std::size_t index) {
    if (index >= row.size() || !row[index].has_value()) {
        throw DatabaseError("database returned an unexpected NULL value");
    }
    return *row[index];
}

} // namespace exchange
