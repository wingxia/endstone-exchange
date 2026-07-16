-- Market rows are immutable item snapshots. A target binding points at the
-- currently active generation and is removed when that market is closed.
CREATE TABLE IF NOT EXISTS exchange_target_bindings (
    target_key VARCHAR(255) NOT NULL PRIMARY KEY,
    market_id BIGINT UNSIGNED NOT NULL UNIQUE,
    bound_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    CONSTRAINT fk_exchange_target_bindings_market
        FOREIGN KEY (market_id) REFERENCES exchange_markets(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT INTO exchange_target_bindings(target_key, market_id)
SELECT target_key, id FROM exchange_markets WHERE active=1
ON DUPLICATE KEY UPDATE market_id=VALUES(market_id);

-- The embedded migration runner conditionally removes the legacy unique
-- target_key index before adding this history index.
CREATE INDEX idx_exchange_markets_target_history ON exchange_markets(target_key, id);

INSERT INTO exchange_schema_versions(version) VALUES (2);
