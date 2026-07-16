-- Crash-recoverable transfer of sell items from a player inventory into the
-- exchange. Inventory markers and receipt tokens are reconciled against this
-- state before an order is created exactly once.
CREATE TABLE IF NOT EXISTS exchange_sell_escrows (
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
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT INTO exchange_schema_versions(version) VALUES (4);
