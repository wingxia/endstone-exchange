#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace exchange {

enum class OrderSide { Buy, Sell };
enum class OrderType { Market, Limit };
enum class OrderStatus { Open, Partial, Filled, Cancelled, Expired, Failed };
enum class SettlementJobType { Credit, Refund };

struct Enchantment {
    std::string id;
    int level{0};
};

struct Product {
    std::string product_key;
    std::string item_id;
    std::vector<Enchantment> enchants;
    std::string display_name;
    std::string category;
    std::string icon;
    bool tradable{true};
    std::vector<std::string> search_terms;
};

struct ItemSnapshot {
    std::string product_key;
    std::string item_id;
    std::vector<Enchantment> enchants;
    std::int32_t quantity{0};
    std::vector<std::uint8_t> nbt_blob;
    std::string nbt_summary;
};

struct Order {
    std::int64_t id{0};
    OrderSide side{OrderSide::Buy};
    OrderType type{OrderType::Limit};
    std::string product_key;
    std::string player_uuid;
    std::string player_name;
    std::int32_t original_quantity{0};
    std::int32_t remaining_quantity{0};
    std::optional<std::int64_t> unit_price;
    std::int64_t reserved_amount{0};
    bool system_order{false};
    OrderStatus status{OrderStatus::Open};
};

struct ItemLot {
    std::int64_t id{0};
    std::int64_t order_id{0};
    std::string product_key;
    std::string owner_uuid;
    std::string owner_name;
    std::int32_t quantity{0};
    std::int32_t remaining_quantity{0};
    std::vector<std::uint8_t> nbt_blob;
    std::string nbt_summary;
    bool system_lot{false};
};

struct ReservedFund {
    std::int64_t id{0};
    std::int64_t order_id{0};
    std::string player_uuid;
    std::string player_name;
    std::int64_t amount_reserved{0};
    std::int64_t amount_remaining{0};
};

struct Trade {
    std::int64_t id{0};
    std::string product_key;
    std::int64_t buy_order_id{0};
    std::int64_t sell_order_id{0};
    std::string buyer_uuid;
    std::string buyer_name;
    std::string seller_uuid;
    std::string seller_name;
    std::int64_t unit_price{0};
    std::int32_t quantity{0};
    std::int64_t total_price{0};
    bool system_sale{false};
};

struct MailboxItem {
    std::int64_t id{0};
    std::string player_uuid;
    std::string player_name;
    std::string product_key;
    std::int32_t quantity{0};
    std::vector<std::uint8_t> nbt_blob;
    std::string nbt_summary;
    bool claimed{false};
};

struct SettlementJob {
    std::int64_t id{0};
    SettlementJobType type{SettlementJobType::Credit};
    std::string player_uuid;
    std::string player_name;
    std::int64_t amount{0};
    std::string reason;
    std::optional<std::int64_t> trade_id;
    std::int32_t attempts{0};
    bool done{false};
};

struct Quote {
    std::string product_key;
    std::optional<std::int64_t> best_bid;
    std::optional<std::int64_t> best_ask;
    std::int32_t bid_quantity{0};
    std::int32_t ask_quantity{0};
    std::optional<std::int64_t> last_price;
};

struct OrderBook {
    std::vector<Order> bids;
    std::vector<Order> asks;
};

struct PlayerRef {
    std::string uuid;
    std::string name;
};

}  // namespace exchange
