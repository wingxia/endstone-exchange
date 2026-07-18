-- Physical targets remain independent market generations, while exact item
-- identities share one durable order book across every world position.
CREATE TABLE IF NOT EXISTS exchange_books (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    item_type VARCHAR(160) NOT NULL,
    item_data INT NOT NULL DEFAULT 0,
    item_nbt LONGBLOB NOT NULL,
    item_name VARCHAR(255) NOT NULL,
    item_hash BINARY(32) NOT NULL,
    created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    UNIQUE KEY uq_exchange_books_item_hash (item_hash)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT IGNORE INTO exchange_books(item_type,item_data,item_nbt,item_name,item_hash)
SELECT item_type,item_data,item_nbt,item_name,
       UNHEX(SHA2(CONCAT(CHAR_LENGTH(item_type),':',item_type,':',item_data,':',
                          OCTET_LENGTH(item_nbt),':',item_nbt),256))
FROM exchange_markets ORDER BY id;

ALTER TABLE exchange_markets ADD COLUMN book_id BIGINT UNSIGNED NULL AFTER id;

UPDATE exchange_markets m JOIN exchange_books b
ON b.item_hash=UNHEX(SHA2(CONCAT(CHAR_LENGTH(m.item_type),':',m.item_type,':',m.item_data,':',
                                 OCTET_LENGTH(m.item_nbt),':',m.item_nbt),256))
SET m.book_id=b.id WHERE m.book_id IS NULL;

ALTER TABLE exchange_markets
    MODIFY COLUMN book_id BIGINT UNSIGNED NOT NULL,
    ADD INDEX idx_exchange_markets_book (book_id,id),
    ADD CONSTRAINT fk_exchange_markets_book FOREIGN KEY (book_id) REFERENCES exchange_books(id);

INSERT INTO exchange_schema_versions(version) VALUES (7);
