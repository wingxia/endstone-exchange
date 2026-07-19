#pragma once

#include "endstone_exchange/domain.hpp"

#include <optional>
#include <string>

namespace exchange {

struct HologramAnchor {
    std::string dimension;
    float x{};
    float y{};
    float z{};

    friend bool operator==(const HologramAnchor &, const HologramAnchor &) = default;
};

[[nodiscard]] int blockToChunk(int block_coordinate) noexcept;
[[nodiscard]] bool sameHologramAnchor(const HologramAnchor &left, const HologramAnchor &right,
                                      float tolerance = 0.01F) noexcept;
[[nodiscard]] bool shouldRepositionHologram(TargetKind target_kind,
                                            const std::optional<HologramAnchor> &previous_anchor,
                                            const HologramAnchor &current_anchor) noexcept;
[[nodiscard]] std::string marketHologramText(const Market &market, const OrderBook &book);

} // namespace exchange
