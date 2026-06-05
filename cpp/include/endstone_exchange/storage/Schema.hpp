#pragma once

#include <string_view>

namespace exchange::storage {

inline constexpr std::string_view kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS exchange_products (
  product_key CHAR(64) PRIMARY KEY,
  item_id VARCHAR(191) NOT NULL,
  enchant_signature TEXT NOT NULL,
  display_name VARCHAR(191) NOT NULL,
  category VARCHAR(96) NOT NULL,
  icon VARCHAR(255) NULL,
  tradable TINYINT(1) NOT NULL DEFAULT 1,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  INDEX idx_exchange_products_category (category, tradable)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS exchange_orders (
  id BIGINT AUTO_INCREMENT PRIMARY KEY,
  side ENUM('buy', 'sell') NOT NULL,
  order_type ENUM('market', 'limit') NOT NULL,
  product_key CHAR(64) NOT NULL,
  player_uuid VARCHAR(64) NOT NULL,
  player_name VARCHAR(64) NOT NULL,
  unit_price BIGINT NULL,
  original_quantity INT NOT NULL,
  remaining_quantity INT NOT NULL,
  reserved_amount BIGINT NOT NULL DEFAULT 0,
  system_order TINYINT(1) NOT NULL DEFAULT 0,
  status VARCHAR(16) NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  INDEX idx_exchange_orders_book (product_key, side, status, unit_price, created_at, id),
  INDEX idx_exchange_orders_player (player_uuid, status, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS exchange_item_lots (
  id BIGINT AUTO_INCREMENT PRIMARY KEY,
  order_id BIGINT NOT NULL,
  product_key CHAR(64) NOT NULL,
  owner_uuid VARCHAR(64) NOT NULL,
  owner_name VARCHAR(64) NOT NULL,
  quantity INT NOT NULL,
  remaining_quantity INT NOT NULL,
  nbt_blob LONGBLOB NOT NULL,
  nbt_summary TEXT NOT NULL,
  system_lot TINYINT(1) NOT NULL DEFAULT 0,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_exchange_lots_order (order_id, remaining_quantity),
  INDEX idx_exchange_lots_product (product_key)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS exchange_reserved_funds (
  id BIGINT AUTO_INCREMENT PRIMARY KEY,
  order_id BIGINT NOT NULL UNIQUE,
  player_uuid VARCHAR(64) NOT NULL,
  player_name VARCHAR(64) NOT NULL,
  amount_reserved BIGINT NOT NULL,
  amount_remaining BIGINT NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS exchange_trades (
  id BIGINT AUTO_INCREMENT PRIMARY KEY,
  product_key CHAR(64) NOT NULL,
  buy_order_id BIGINT NOT NULL,
  sell_order_id BIGINT NOT NULL,
  buyer_uuid VARCHAR(64) NOT NULL,
  buyer_name VARCHAR(64) NOT NULL,
  seller_uuid VARCHAR(64) NOT NULL,
  seller_name VARCHAR(64) NOT NULL,
  unit_price BIGINT NOT NULL,
  quantity INT NOT NULL,
  total_price BIGINT NOT NULL,
  system_sale TINYINT(1) NOT NULL DEFAULT 0,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_exchange_trades_product (product_key, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS exchange_trade_items (
  id BIGINT AUTO_INCREMENT PRIMARY KEY,
  trade_id BIGINT NOT NULL,
  source_lot_id BIGINT NOT NULL,
  quantity INT NOT NULL,
  nbt_blob LONGBLOB NOT NULL,
  nbt_summary TEXT NOT NULL,
  INDEX idx_exchange_trade_items_trade (trade_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS exchange_mailbox (
  id BIGINT AUTO_INCREMENT PRIMARY KEY,
  player_uuid VARCHAR(64) NOT NULL,
  player_name VARCHAR(64) NOT NULL,
  product_key CHAR(64) NOT NULL,
  quantity INT NOT NULL,
  nbt_blob LONGBLOB NOT NULL,
  nbt_summary TEXT NOT NULL,
  status VARCHAR(16) NOT NULL DEFAULT 'pending',
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  claimed_at TIMESTAMP NULL,
  INDEX idx_exchange_mailbox_player (player_uuid, status, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS exchange_settlement_jobs (
  id BIGINT AUTO_INCREMENT PRIMARY KEY,
  job_type ENUM('credit', 'refund') NOT NULL,
  player_uuid VARCHAR(64) NOT NULL,
  player_name VARCHAR(64) NOT NULL,
  amount BIGINT NOT NULL,
  reason VARCHAR(255) NULL,
  trade_id BIGINT NULL,
  status VARCHAR(16) NOT NULL DEFAULT 'pending',
  attempts INT NOT NULL DEFAULT 0,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  INDEX idx_exchange_settlement_status (status, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS exchange_admin_actions (
  id BIGINT AUTO_INCREMENT PRIMARY KEY,
  admin_uuid VARCHAR(64) NOT NULL,
  admin_name VARCHAR(64) NOT NULL,
  action VARCHAR(64) NOT NULL,
  product_key CHAR(64) NULL,
  order_id BIGINT NULL,
  quantity INT NULL,
  unit_price BIGINT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_exchange_admin_actions_admin (admin_uuid, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
)SQL";

}  // namespace exchange::storage

