#include "endstone_exchange/market_display.hpp"

#include <cmath>
#include <format>

namespace exchange {
namespace {

std::string formatMoney(const Cents cents) {
    return std::format("{:.2f}", static_cast<double>(cents) / 100.0);
}

} // namespace

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
    // Block labels rest on the target block and never need a periodic teleport. Actor labels
    // only move when their target actually changes position, not while their armor stand settles.
    return target_kind == TargetKind::Actor &&
           (!previous_anchor || !sameHologramAnchor(*previous_anchor, current_anchor));
}

std::string marketHologramText(const Market &market, const OrderBook &book) {
    const auto wanted_price = book.bids.empty() ? std::string("--") : formatMoney(book.bids.front().price_cents);
    const auto offered_price = book.asks.empty() ? std::string("--") : formatMoney(book.asks.front().price_cents);
    const auto wanted_quantity = book.bids.empty() ? 0 : book.bids.front().quantity;
    const auto offered_quantity = book.asks.empty() ? 0 : book.asks.front().quantity;
    return std::format("§6{}§r\n§a收购 {} × {}§r\n§c出售 {} × {}§r\n§e右击交易§r", market.item.name,
                       wanted_price, wanted_quantity, offered_price, offered_quantity);
}

} // namespace exchange
