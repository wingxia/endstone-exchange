-- Manual exchanger closes pause a market without canceling its order book.
-- Reopening the same target with the exact same item snapshot resumes this
-- generation; target destruction still retires it and settles every order.
ALTER TABLE exchange_markets
    ADD COLUMN reopenable BOOLEAN NOT NULL DEFAULT FALSE AFTER active;

INSERT INTO exchange_schema_versions(version) VALUES (6);
