-- Durable cross-system transfers between UMoney and the exchange balance.
-- A transfer is prepared before either side is changed. The bridge applies
-- the external operation idempotently, then the exchange completes its local
-- balance mutation in a separate recoverable state transition.
CREATE TABLE IF NOT EXISTS exchange_economy_transfers (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    operation_key CHAR(36) NOT NULL,
    player_uuid CHAR(36) NOT NULL,
    player_name VARCHAR(64) NOT NULL,
    direction ENUM('DEPOSIT','WITHDRAW') NOT NULL,
    amount_cents BIGINT NOT NULL,
    amount_units BIGINT NOT NULL,
    status ENUM('PREPARED','EXTERNAL_APPLIED','COMPLETED','FAILED','BLOCKED')
        NOT NULL DEFAULT 'PREPARED',
    external_balance_units BIGINT NULL,
    attempt_count INT UNSIGNED NOT NULL DEFAULT 0,
    last_error VARCHAR(512) NULL,
    created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    external_applied_at TIMESTAMP(6) NULL,
    completed_at TIMESTAMP(6) NULL,
    updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
        ON UPDATE CURRENT_TIMESTAMP(6),
    UNIQUE KEY uq_exchange_economy_transfers_operation (operation_key),
    INDEX idx_exchange_economy_transfers_pending (status,id),
    INDEX idx_exchange_economy_transfers_player (player_uuid,id),
    CONSTRAINT fk_exchange_economy_transfers_account
        FOREIGN KEY (player_uuid) REFERENCES exchange_accounts(player_uuid),
    CHECK (amount_cents > 0),
    CHECK (amount_units > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT INTO exchange_schema_versions(version) VALUES (8);
