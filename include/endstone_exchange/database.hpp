#pragma once

#include "endstone_exchange/config.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace exchange {

class DatabaseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using QueryCell = std::optional<std::string>;
using QueryRow = std::vector<QueryCell>;
using QueryRows = std::vector<QueryRow>;

class Database {
public:
    explicit Database(DatabaseConfig config);
    ~Database();

    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
    Database(Database &&) noexcept;
    Database &operator=(Database &&) noexcept;

    void connect();
    void migrate();
    void ping();

    void execute(std::string_view sql);
    [[nodiscard]] QueryRows query(std::string_view sql);
    [[nodiscard]] std::uint64_t lastInsertId() const;
    [[nodiscard]] std::uint64_t affectedRows() const;

    void begin();
    void commit();
    void rollback() noexcept;

    [[nodiscard]] std::string quote(std::string_view value) const;
    [[nodiscard]] static std::string hexLiteral(const std::vector<std::uint8_t> &value);
    [[nodiscard]] const DatabaseConfig &config() const noexcept { return config_; }

private:
    struct Impl;
    DatabaseConfig config_;
    std::unique_ptr<Impl> impl_;

    void ensureConnected() const;
};

class Transaction {
public:
    explicit Transaction(Database &database);
    ~Transaction();
    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

    void commit();

private:
    Database &database_;
    bool committed_{false};
};

[[nodiscard]] std::int64_t cellInt64(const QueryRow &row, std::size_t index);
[[nodiscard]] int cellInt(const QueryRow &row, std::size_t index);
[[nodiscard]] std::string cellString(const QueryRow &row, std::size_t index);

}  // namespace exchange
