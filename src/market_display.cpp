#include "endstone_exchange/market_display.hpp"

#include <cmath>
#include <format>

namespace exchange {
int blockToChunk(const int block_coordinate) noexcept {
    if (block_coordinate >= 0) {
        return block_coordinate / 16;
    }
    return -(((-block_coordinate) + 15) / 16);
}

bool sameHologramAnchor(const HologramAnchor &left, const HologramAnchor &right, const float tolerance) noexcept {
    return left.dimension == right.dimension && std::fabs(left.x - right.x) <= tolerance &&
           std::fabs(left.y - right.y) <= tolerance && std::fabs(left.z - right.z) <= tolerance;
}

bool shouldRepositionHologram(const TargetKind target_kind,
                              const std::optional<HologramAnchor> &previous_anchor,
                              const HologramAnchor &current_anchor) noexcept {
    // An adopted block label may need one correction after its intended anchor changes. Once
    // remembered, block labels are not teleported just because their armor stand settles.
    if (!previous_anchor) {
        return target_kind == TargetKind::Actor;
    }
    return !sameHologramAnchor(*previous_anchor, current_anchor);
}

std::optional<BlockOffset> itemFrameSupportOffset(const int facing_direction) noexcept {
    // Bedrock facing_direction points away from the supporting block.
    switch (facing_direction) {
    case 0:
        return BlockOffset{0, 1, 0};
    case 1:
        return BlockOffset{0, -1, 0};
    case 2:
        return BlockOffset{0, 0, 1};
    case 3:
        return BlockOffset{0, 0, -1};
    case 4:
        return BlockOffset{1, 0, 0};
    case 5:
        return BlockOffset{-1, 0, 0};
    default:
        return std::nullopt;
    }
}

std::string formatUnitPrice(const Cents cents) {
    return std::format("{}u", cents / 100);
}

std::string marketHologramText(const Market &market, const OrderBook &book, const Language language,
                               const std::string_view localized_item_name) {
    const auto wanted_price = book.bids.empty() ? std::string("--") : formatUnitPrice(book.bids.front().price_cents);
    const auto offered_price = book.asks.empty() ? std::string("--") : formatUnitPrice(book.asks.front().price_cents);
    const auto wanted_quantity = book.bids.empty() ? 0 : book.bids.front().quantity;
    const auto offered_quantity = book.asks.empty() ? 0 : book.asks.front().quantity;
    const auto item_name = localized_item_name.empty() ? std::string_view(market.item.name) : localized_item_name;
    return std::format("§6{}§r\n§a{} {} x {}§r\n§c{} {} x {}§r", item_name,
                       messageText(language, Message::HologramWanted), wanted_price, wanted_quantity,
                       messageText(language, Message::HologramForSale), offered_price, offered_quantity);
}

} // namespace exchange
