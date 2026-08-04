CREATE TABLE IF NOT EXISTS exchange_frame_listings (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    target_key VARCHAR(255) NOT NULL,
    dimension_name VARCHAR(96) NOT NULL,
    block_x INT NOT NULL,
    block_y INT NOT NULL,
    block_z INT NOT NULL,
    seller_uuid CHAR(36) NOT NULL,
    seller_name VARCHAR(64) NOT NULL,
    price_cents BIGINT NOT NULL,
    item_type VARCHAR(160) NOT NULL,
    item_data INT NOT NULL DEFAULT 0,
    item_nbt LONGBLOB NOT NULL,
    item_name VARCHAR(255) NOT NULL,
    status ENUM('ACTIVE','PAID','DROPPED','CLAIMED','CANCELED') NOT NULL DEFAULT 'ACTIVE',
    buyer_uuid CHAR(36) NULL,
    buyer_name VARCHAR(64) NULL,
    created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    paid_at TIMESTAMP(6) NULL,
    dropped_at TIMESTAMP(6) NULL,
    claimed_at TIMESTAMP(6) NULL,
    canceled_at TIMESTAMP(6) NULL,
    updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
    INDEX idx_exchange_frame_listings_target (target_key,id),
    INDEX idx_exchange_frame_listings_unsettled (status,id),
    INDEX idx_exchange_frame_listings_seller (seller_uuid,id),
    INDEX idx_exchange_frame_listings_buyer (buyer_uuid,id),
    CONSTRAINT fk_exchange_frame_listings_seller
        FOREIGN KEY (seller_uuid) REFERENCES exchange_accounts(player_uuid),
    CONSTRAINT fk_exchange_frame_listings_buyer
        FOREIGN KEY (buyer_uuid) REFERENCES exchange_accounts(player_uuid),
    CHECK (price_cents > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS exchange_frame_listing_bindings (
    target_key VARCHAR(255) NOT NULL PRIMARY KEY,
    listing_id BIGINT UNSIGNED NOT NULL UNIQUE,
    bound_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    CONSTRAINT fk_exchange_frame_listing_bindings_listing
        FOREIGN KEY (listing_id) REFERENCES exchange_frame_listings(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT INTO exchange_schema_versions(version) VALUES (9);
