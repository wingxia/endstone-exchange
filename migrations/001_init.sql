CREATE TABLE IF NOT EXISTS exchange_schema_versions (
    version INT NOT NULL PRIMARY KEY,
    applied_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS exchange_accounts (
    player_uuid CHAR(36) NOT NULL PRIMARY KEY,
    player_name VARCHAR(64) NOT NULL,
    balance_cents BIGINT NOT NULL,
    created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
    CHECK (balance_cents >= 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS exchange_markets (
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
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS exchange_orders (
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
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS exchange_trades (
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
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS exchange_deliveries (
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
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT IGNORE INTO exchange_schema_versions(version) VALUES (1);
