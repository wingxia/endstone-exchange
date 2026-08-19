#pragma once

#include "endstone_exchange/domain.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace exchange {

inline constexpr Cents BulkSellListingPriceCents = 100;

struct BulkSellCandidate {
    Id market_id{0};
    Id book_id{0};
    int quantity{0};
};

struct BulkSellBatch {
    Id market_id{0};
    Id book_id{0};
    int quantity{0};
};

struct BulkSellPlan {
    std::size_t item_kinds{0};
    int total_quantity{0};
    std::vector<BulkSellBatch> batches;
};

[[nodiscard]] bool isBulkSellTerminal(std::string_view block_type) noexcept;
[[nodiscard]] BulkSellPlan makeBulkSellPlan(const std::vector<BulkSellCandidate> &candidates,
                                            int max_order_quantity);

} // namespace exchange
