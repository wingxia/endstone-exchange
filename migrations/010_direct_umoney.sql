ALTER TABLE exchange_economy_transfers
    ADD COLUMN IF NOT EXISTS external_balance_before_units BIGINT NULL AFTER status;

INSERT INTO exchange_schema_versions(version) VALUES (10);
