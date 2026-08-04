#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace exchange {

using Cents = std::int64_t;
using Id = std::uint64_t;

enum class TargetKind { Block, Actor };
enum class Side { Buy, Sell };
enum class OrderType { Limit, Market };
enum class DeliveryClaimStatus { Prepared, Applied, Canceled };
enum class SellEscrowStatus { Prepared, Tagged, Ordered, Canceled };
enum class EconomyTransferDirection { Deposit, Withdraw };
enum class EconomyTransferStatus { Prepared, ExternalApplied, Completed, Failed, Blocked };
enum class FrameListingStatus { Active, Paid, Dropped, Claimed, Canceled };

struct ItemPrototype {
    std::string type;
    int data{0};
    std::vector<std::uint8_t> nbt;
    std::string name;
};

struct Market {
    Id id{0};
    Id book_id{0};
    std::string target_key;
    TargetKind target_kind{TargetKind::Block};
    std::string dimension_name;
    std::optional<int> block_x;
    std::optional<int> block_y;
    std::optional<int> block_z;
    std::optional<std::int64_t> actor_id;
    ItemPrototype item;
    bool active{true};
    std::string created_by;
};

struct PriceLevel {
    Cents price_cents{0};
    int quantity{0};
};

struct OrderBook {
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    std::optional<Cents> last_price_cents;
};

struct OrderRequest {
    Id market_id{0};
    std::string player_uuid;
    std::string player_name;
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    Cents price_cents{0};
    int quantity{0};
};

struct ExecutionResult {
    Id order_id{0};
    int requested_quantity{0};
    int filled_quantity{0};
    int open_quantity{0};
    Cents gross_cents{0};
    Cents balance_cents{0};
};

struct OpenOrder {
    Id id{0};
    Id market_id{0};
    ItemPrototype item;
    Side side{Side::Buy};
    Cents price_cents{0};
    int remaining_quantity{0};
};

struct Delivery {
    Id id{0};
    Id market_id{0};
    ItemPrototype item;
    int quantity{0};
    int claimed_quantity{0};
    int reserved_quantity{0};
    std::string reason;
};

struct DeliveryClaim {
    Id id{0};
    Id delivery_id{0};
    ItemPrototype item;
    int quantity{0};
    int applied_quantity{0};
    DeliveryClaimStatus status{DeliveryClaimStatus::Prepared};
    bool cleaned{false};
};

struct SellEscrow {
    Id id{0};
    Id market_id{0};
    ItemPrototype item;
    std::string player_uuid;
    std::string player_name;
    OrderType order_type{OrderType::Limit};
    Cents price_cents{0};
    int requested_quantity{0};
    int tagged_quantity{0};
    int receipt_count{0};
    SellEscrowStatus status{SellEscrowStatus::Prepared};
    std::optional<Id> order_id;
    bool cleaned{false};
};

struct EconomyTransfer {
    Id id{0};
    std::string operation_key;
    std::string player_uuid;
    std::string player_name;
    EconomyTransferDirection direction{EconomyTransferDirection::Deposit};
    Cents amount_cents{0};
    std::int64_t amount_units{0};
    EconomyTransferStatus status{EconomyTransferStatus::Prepared};
    std::optional<std::int64_t> external_balance_before_units;
    std::optional<std::int64_t> external_balance_units;
    int attempt_count{0};
    std::string last_error;
};

struct AccountBalance {
    std::string player_uuid;
    std::string player_name;
    Cents balance_cents{0};
};

struct FrameListing {
    Id id{0};
    std::string target_key;
    std::string dimension_name;
    int block_x{0};
    int block_y{0};
    int block_z{0};
    std::string seller_uuid;
    std::string seller_name;
    Cents price_cents{0};
    ItemPrototype item;
    FrameListingStatus status{FrameListingStatus::Active};
    std::optional<std::string> buyer_uuid;
    std::optional<std::string> buyer_name;
};

struct FramePurchaseResult {
    FrameListing listing;
    Cents buyer_balance_cents{0};
    Cents seller_balance_cents{0};
};

[[nodiscard]] inline const char *toSql(TargetKind kind) {
    return kind == TargetKind::Block ? "BLOCK" : "ACTOR";
}

[[nodiscard]] inline const char *toSql(Side side) {
    return side == Side::Buy ? "BUY" : "SELL";
}

[[nodiscard]] inline const char *toSql(OrderType type) {
    return type == OrderType::Limit ? "LIMIT" : "MARKET";
}

[[nodiscard]] inline const char *toSql(EconomyTransferDirection direction) {
    return direction == EconomyTransferDirection::Deposit ? "DEPOSIT" : "WITHDRAW";
}

} // namespace exchange
