#include "endstone_exchange/bulk_sell.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>

namespace exchange {

bool isBulkSellTerminal(const std::string_view block_type) noexcept {
    return block_type == "minecraft:command_block" || block_type == "minecraft:chain_command_block" ||
           block_type == "minecraft:repeating_command_block";
}

BulkSellPlan makeBulkSellPlan(const std::vector<BulkSellCandidate> &candidates,
                              const int max_order_quantity) {
    if (max_order_quantity <= 0) {
        throw std::invalid_argument("bulk-sell order limit must be positive");
    }

    std::map<Id, BulkSellCandidate> by_book;
    for (const auto &candidate : candidates) {
        if (candidate.quantity < 0) {
            throw std::invalid_argument("bulk-sell quantity must not be negative");
        }
        if (candidate.quantity == 0) {
            continue;
        }
        if (candidate.market_id == 0 || candidate.book_id == 0) {
            throw std::invalid_argument("bulk-sell candidate identity is incomplete");
        }
        const auto [it, inserted] = by_book.try_emplace(candidate.book_id, candidate);
        if (!inserted) {
            if (it->second.quantity != candidate.quantity) {
                throw std::runtime_error("shared bulk-sell markets disagree on inventory quantity");
            }
            it->second.market_id = std::min(it->second.market_id, candidate.market_id);
        }
    }

    std::vector<BulkSellCandidate> selected;
    selected.reserve(by_book.size());
    for (const auto &[book_id, candidate] : by_book) {
        static_cast<void>(book_id);
        selected.push_back(candidate);
    }
    std::ranges::sort(selected, {}, &BulkSellCandidate::market_id);

    BulkSellPlan plan;
    plan.item_kinds = selected.size();
    for (const auto &candidate : selected) {
        if (candidate.quantity > std::numeric_limits<int>::max() - plan.total_quantity) {
            throw std::overflow_error("bulk-sell inventory quantity is too large");
        }
        plan.total_quantity += candidate.quantity;
        int remaining = candidate.quantity;
        while (remaining > 0) {
            const auto quantity = std::min(remaining, max_order_quantity);
            plan.batches.push_back({candidate.market_id, candidate.book_id, quantity});
            remaining -= quantity;
        }
    }
    return plan;
}

} // namespace exchange
