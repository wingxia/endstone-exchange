-- Immutable audit trail for every exchange-balance mutation. Existing
-- accounts receive one opening entry so SUM(delta_cents) equals the account.
CREATE TABLE IF NOT EXISTS exchange_balance_ledger (
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
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT INTO exchange_balance_ledger(player_uuid,delta_cents,reason)
SELECT a.player_uuid,a.balance_cents,'MIGRATION_OPENING_BALANCE' FROM exchange_accounts a
WHERE NOT EXISTS (SELECT 1 FROM exchange_balance_ledger l WHERE l.player_uuid=a.player_uuid);

INSERT INTO exchange_schema_versions(version) VALUES (5);
