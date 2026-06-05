#include "endstone_exchange/core/Repository.hpp"

#include <algorithm>

namespace exchange {

bool isOpen(OrderStatus status) {
    return status == OrderStatus::Open || status == OrderStatus::Partial;
}

void InMemoryRepository::upsertProduct(const Product& product) {
    auto it = std::find_if(products_.begin(), products_.end(), [&](const Product& current) {
        return current.product_key == product.product_key;
    });
    if (it == products_.end()) {
        products_.push_back(product);
    } else {
        *it = product;
    }
}

std::optional<Product> InMemoryRepository::getProduct(const std::string& product_key) const {
    auto it = std::find_if(products_.begin(), products_.end(), [&](const Product& product) {
        return product.product_key == product_key;
    });
    if (it == products_.end()) {
        return std::nullopt;
    }
    return *it;
}

std::vector<Product> InMemoryRepository::listProducts(const std::optional<std::string>& category) const {
    std::vector<Product> out;
    for (const auto& product : products_) {
        if (!product.tradable) {
            continue;
        }
        if (category && product.category != *category) {
            continue;
        }
        out.push_back(product);
    }
    std::sort(out.begin(), out.end(), [](const Product& lhs, const Product& rhs) {
        return std::tie(lhs.category, lhs.display_name, lhs.item_id) < std::tie(rhs.category, rhs.display_name, rhs.item_id);
    });
    return out;
}

Order InMemoryRepository::createOrder(Order order) {
    order.id = next_order_id_++;
    orders_.push_back(order);
    return order;
}

void InMemoryRepository::updateOrder(const Order& order) {
    auto it = std::find_if(orders_.begin(), orders_.end(), [&](const Order& current) { return current.id == order.id; });
    if (it != orders_.end()) {
        *it = order;
    }
}

std::optional<Order> InMemoryRepository::getOrder(std::int64_t order_id) const {
    auto it = std::find_if(orders_.begin(), orders_.end(), [&](const Order& order) { return order.id == order_id; });
    if (it == orders_.end()) {
        return std::nullopt;
    }
    return *it;
}

std::vector<Order> InMemoryRepository::matchingSellOrders(const std::string& product_key, std::optional<std::int64_t> max_price, std::size_t limit) const {
    std::vector<Order> out;
    for (const auto& order : orders_) {
        if (order.product_key != product_key || order.side != OrderSide::Sell || !isOpen(order.status) || order.remaining_quantity <= 0 || !order.unit_price) {
            continue;
        }
        if (max_price && *order.unit_price > *max_price) {
            continue;
        }
        out.push_back(order);
    }
    std::sort(out.begin(), out.end(), [](const Order& lhs, const Order& rhs) {
        return std::tie(*lhs.unit_price, lhs.id) < std::tie(*rhs.unit_price, rhs.id);
    });
    if (out.size() > limit) {
        out.resize(limit);
    }
    return out;
}

std::vector<Order> InMemoryRepository::matchingBuyOrders(const std::string& product_key, std::optional<std::int64_t> min_price, std::size_t limit) const {
    std::vector<Order> out;
    for (const auto& order : orders_) {
        if (order.product_key != product_key || order.side != OrderSide::Buy || !isOpen(order.status) || order.remaining_quantity <= 0 || !order.unit_price) {
            continue;
        }
        if (min_price && *order.unit_price < *min_price) {
            continue;
        }
        out.push_back(order);
    }
    std::sort(out.begin(), out.end(), [](const Order& lhs, const Order& rhs) {
        return std::tie(*rhs.unit_price, lhs.id) < std::tie(*lhs.unit_price, rhs.id);
    });
    if (out.size() > limit) {
        out.resize(limit);
    }
    return out;
}

std::vector<Order> InMemoryRepository::playerOrders(const std::string& player_uuid, bool include_closed) const {
    std::vector<Order> out;
    for (const auto& order : orders_) {
        if (order.player_uuid != player_uuid) {
            continue;
        }
        if (!include_closed && !isOpen(order.status)) {
            continue;
        }
        out.push_back(order);
    }
    std::sort(out.begin(), out.end(), [](const Order& lhs, const Order& rhs) { return lhs.id > rhs.id; });
    return out;
}

ItemLot InMemoryRepository::createItemLot(ItemLot lot) {
    lot.id = next_lot_id_++;
    lots_.push_back(lot);
    return lot;
}

void InMemoryRepository::updateItemLot(const ItemLot& lot) {
    auto it = std::find_if(lots_.begin(), lots_.end(), [&](const ItemLot& current) { return current.id == lot.id; });
    if (it != lots_.end()) {
        *it = lot;
    }
}

std::vector<ItemLot> InMemoryRepository::lotsForOrder(std::int64_t order_id) const {
    std::vector<ItemLot> out;
    for (const auto& lot : lots_) {
        if (lot.order_id == order_id && lot.remaining_quantity > 0) {
            out.push_back(lot);
        }
    }
    std::sort(out.begin(), out.end(), [](const ItemLot& lhs, const ItemLot& rhs) { return lhs.id < rhs.id; });
    return out;
}

ReservedFund InMemoryRepository::createReservedFund(ReservedFund fund) {
    fund.id = next_fund_id_++;
    reserved_funds_.push_back(fund);
    return fund;
}

void InMemoryRepository::updateReservedFund(const ReservedFund& fund) {
    auto it = std::find_if(reserved_funds_.begin(), reserved_funds_.end(), [&](const ReservedFund& current) { return current.id == fund.id; });
    if (it != reserved_funds_.end()) {
        *it = fund;
    }
}

std::optional<ReservedFund> InMemoryRepository::reservedFundForOrder(std::int64_t order_id) const {
    auto it = std::find_if(reserved_funds_.begin(), reserved_funds_.end(), [&](const ReservedFund& fund) {
        return fund.order_id == order_id;
    });
    if (it == reserved_funds_.end()) {
        return std::nullopt;
    }
    return *it;
}

Trade InMemoryRepository::createTrade(Trade trade) {
    trade.id = next_trade_id_++;
    trades_.push_back(trade);
    return trade;
}

std::vector<Trade> InMemoryRepository::tradesForProduct(const std::string& product_key) const {
    std::vector<Trade> out;
    for (const auto& trade : trades_) {
        if (trade.product_key == product_key) {
            out.push_back(trade);
        }
    }
    return out;
}

void InMemoryRepository::addTradeItem(std::int64_t, std::int64_t, std::int32_t, std::vector<std::uint8_t>, std::string) {}

void InMemoryRepository::addMailboxItem(MailboxItem item) {
    item.id = next_mailbox_id_++;
    mailbox_.push_back(std::move(item));
}

std::vector<MailboxItem> InMemoryRepository::mailboxForPlayer(const std::string& player_uuid) const {
    std::vector<MailboxItem> out;
    for (const auto& item : mailbox_) {
        if (item.player_uuid == player_uuid && !item.claimed) {
            out.push_back(item);
        }
    }
    return out;
}

SettlementJob InMemoryRepository::createSettlementJob(SettlementJob job) {
    job.id = next_job_id_++;
    jobs_.push_back(job);
    return job;
}

void InMemoryRepository::updateSettlementJob(const SettlementJob& job) {
    auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const SettlementJob& current) { return current.id == job.id; });
    if (it != jobs_.end()) {
        *it = job;
    }
}

std::vector<SettlementJob> InMemoryRepository::pendingSettlementJobs(std::size_t limit) const {
    std::vector<SettlementJob> out;
    for (const auto& job : jobs_) {
        if (!job.done && job.attempts < 5) {
            out.push_back(job);
        }
    }
    std::sort(out.begin(), out.end(), [](const SettlementJob& lhs, const SettlementJob& rhs) { return lhs.id < rhs.id; });
    if (out.size() > limit) {
        out.resize(limit);
    }
    return out;
}

}  // namespace exchange

