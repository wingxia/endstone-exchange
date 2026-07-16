-- Two-phase inventory delivery. PREPARED quantities are reserved before an
-- item with the claim id in hidden NBT enters the player's inventory. On
-- reconnect the marker decides whether to apply or release the reservation.
ALTER TABLE exchange_deliveries
    ADD COLUMN reserved_qty INT NOT NULL DEFAULT 0 AFTER claimed_qty,
    ADD CONSTRAINT chk_exchange_deliveries_claims
        CHECK (claimed_qty >= 0 AND reserved_qty >= 0 AND claimed_qty + reserved_qty <= quantity);

CREATE TABLE IF NOT EXISTS exchange_delivery_claims (
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
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT INTO exchange_schema_versions(version) VALUES (3);
