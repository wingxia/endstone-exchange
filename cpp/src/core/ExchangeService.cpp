#include "endstone_exchange/core/ExchangeService.hpp"

#include <algorithm>
#include <sstream>

namespace exchange {
namespace {

constexpr const char* kSystemUuid = "00000000-0000-0000-0000-exchange";
constexpr const char* kSystemName = "ExchangeSystem";

std::string keyFor(const std::string& prefix, std::int64_t id) {
    return prefix + ":" + std::to_string(id);
}

}  // namespace

ExchangeService::ExchangeService(Repository& repository, Economy& economy) : repository_(repository), economy_(economy) {}

std::vector<Product> ExchangeService::getCatalog(const std::optional<std::string>& category) const {
    return repository_.listProducts(category);
}

Quote ExchangeService::getQuote(const std::string& product_key) const {
    auto book = listOrderBook(product_key, 1);
    auto trades = repository_.tradesForProduct(product_key);
    Quote quote;
    quote.product_key = product_key;
    if (!book.bids.empty()) {
        quote.best_bid = book.bids.front().unit_price;
    }
    if (!book.asks.empty()) {
        quote.best_ask = book.asks.front().unit_price;
    }
    for (const auto& order : repository_.matchingBuyOrders(product_key, std::nullopt, 500)) {
        quote.bid_quantity += order.remaining_quantity;
    }
    for (const auto& order : repository_.matchingSellOrders(product_key, std::nullopt, 500)) {
        quote.ask_quantity += order.remaining_quantity;
    }
    if (!trades.empty()) {
        quote.last_price = trades.back().unit_price;
    }
    return quote;
}

OrderBook ExchangeService::listOrderBook(const std::string& product_key, std::size_t depth) const {
    return OrderBook{
        repository_.matchingBuyOrders(product_key, std::nullopt, depth),
        repository_.matchingSellOrders(product_key, std::nullopt, depth)};
}

std::vector<Order> ExchangeService::listPlayerOrders(const PlayerRef& player, bool include_closed) const {
    return repository_.playerOrders(player.uuid, include_closed);
}

Order ExchangeService::placeLimitBuy(const PlayerRef& player, const std::string& product_key, std::int32_t quantity, std::int64_t unit_price) {
    validateProduct(product_key);
    validateQuantity(quantity);
    validatePrice(unit_price);
    const auto reserved = static_cast<std::int64_t>(quantity) * unit_price;
    economy_.debit(player.name, reserved, "limit-buy:" + player.uuid + ":" + product_key + ":" + std::to_string(quantity) + ":" + std::to_string(unit_price));

    auto order = repository_.createOrder(Order{
        0, OrderSide::Buy, OrderType::Limit, product_key, player.uuid, player.name,
        quantity, quantity, unit_price, reserved, false, OrderStatus::Open});
    repository_.createReservedFund(ReservedFund{0, order.id, player.uuid, player.name, reserved, reserved});
    matchBuyOrder(order);
    processSettlements();
    return *repository_.getOrder(order.id);
}

Order ExchangeService::placeMarketBuy(const PlayerRef& player, const std::string& product_key, std::int32_t quantity) {
    validateProduct(product_key);
    validateQuantity(quantity);
    const auto [fillable, total_cost] = estimateMarketBuy(product_key, quantity);
    if (fillable <= 0 || total_cost <= 0) {
        throw InvalidOrder("no sell orders are available");
    }
    economy_.debit(player.name, total_cost, "market-buy:" + player.uuid + ":" + product_key + ":" + std::to_string(quantity));
    auto order = repository_.createOrder(Order{
        0, OrderSide::Buy, OrderType::Market, product_key, player.uuid, player.name,
        fillable, fillable, std::nullopt, total_cost, false, OrderStatus::Open});
    repository_.createReservedFund(ReservedFund{0, order.id, player.uuid, player.name, total_cost, total_cost});
    matchBuyOrder(order);
    if (order.remaining_quantity > 0) {
        refundRemainingReservation(order, "market_buy_unfilled");
        order.remaining_quantity = 0;
        order.status = OrderStatus::Expired;
        repository_.updateOrder(order);
    }
    processSettlements();
    return *repository_.getOrder(order.id);
}

Order ExchangeService::placeLimitSell(const PlayerRef& player, const std::string& product_key, std::vector<ItemSnapshot> snapshots, std::int64_t unit_price) {
    validateProduct(product_key);
    validateSnapshots(product_key, snapshots);
    validatePrice(unit_price);
    std::int32_t quantity = 0;
    for (const auto& snapshot : snapshots) {
        quantity += snapshot.quantity;
    }
    auto order = repository_.createOrder(Order{
        0, OrderSide::Sell, OrderType::Limit, product_key, player.uuid, player.name,
        quantity, quantity, unit_price, 0, false, OrderStatus::Open});
    for (auto& snapshot : snapshots) {
        repository_.createItemLot(ItemLot{
            0, order.id, product_key, player.uuid, player.name, snapshot.quantity, snapshot.quantity,
            std::move(snapshot.nbt_blob), std::move(snapshot.nbt_summary), false});
    }
    matchSellOrder(order);
    processSettlements();
    return *repository_.getOrder(order.id);
}

Order ExchangeService::placeMarketSell(const PlayerRef& player, const std::string& product_key, std::vector<ItemSnapshot> snapshots) {
    validateProduct(product_key);
    validateSnapshots(product_key, snapshots);
    std::int32_t quantity = 0;
    for (const auto& snapshot : snapshots) {
        quantity += snapshot.quantity;
    }
    const auto [fillable, _income] = estimateMarketSell(product_key, quantity);
    if (fillable <= 0) {
        throw InvalidOrder("no buy orders are available");
    }
    if (fillable < quantity) {
        std::int32_t keep = fillable;
        std::vector<ItemSnapshot> reduced;
        for (auto snapshot : snapshots) {
            if (keep <= 0) {
                break;
            }
            if (snapshot.quantity > keep) {
                snapshot.quantity = keep;
            }
            keep -= snapshot.quantity;
            reduced.push_back(std::move(snapshot));
        }
        snapshots = std::move(reduced);
        quantity = fillable;
    }
    auto order = repository_.createOrder(Order{
        0, OrderSide::Sell, OrderType::Market, product_key, player.uuid, player.name,
        quantity, quantity, std::nullopt, 0, false, OrderStatus::Open});
    for (auto& snapshot : snapshots) {
        repository_.createItemLot(ItemLot{
            0, order.id, product_key, player.uuid, player.name, snapshot.quantity, snapshot.quantity,
            std::move(snapshot.nbt_blob), std::move(snapshot.nbt_summary), false});
    }
    matchSellOrder(order);
    if (order.remaining_quantity > 0) {
        returnRemainingLots(order, OrderStatus::Expired);
    }
    processSettlements();
    return *repository_.getOrder(order.id);
}

Order ExchangeService::cancelOrder(const PlayerRef& player, std::int64_t order_id, bool admin) {
    auto order = repository_.getOrder(order_id);
    if (!order) {
        throw InvalidOrder("order not found");
    }
    if (!admin && order->player_uuid != player.uuid) {
        throw InvalidOrder("cannot cancel another player's order");
    }
    if (!isOpen(order->status)) {
        throw InvalidOrder("order is not open");
    }
    if (order->side == OrderSide::Buy) {
        refundRemainingReservation(*order, "cancel_order");
        order->remaining_quantity = 0;
        order->status = OrderStatus::Cancelled;
        repository_.updateOrder(*order);
    } else {
        returnRemainingLots(*order, OrderStatus::Cancelled);
    }
    processSettlements();
    return *repository_.getOrder(order_id);
}

Order ExchangeService::adminCreateSystemSellOrder(const PlayerRef&, const std::string& product_key, std::int32_t quantity, std::int64_t unit_price, std::vector<std::uint8_t> nbt_template) {
    validateProduct(product_key);
    validateQuantity(quantity);
    validatePrice(unit_price);
    auto order = repository_.createOrder(Order{
        0, OrderSide::Sell, OrderType::Limit, product_key, kSystemUuid, kSystemName,
        quantity, quantity, unit_price, 0, true, OrderStatus::Open});
    repository_.createItemLot(ItemLot{
        0, order.id, product_key, kSystemUuid, kSystemName, quantity, quantity,
        std::move(nbt_template), "system-stock", true});
    return *repository_.getOrder(order.id);
}

Order ExchangeService::adminCancelSystemOrder(const PlayerRef& admin, std::int64_t order_id) {
    return cancelOrder(admin, order_id, true);
}

void ExchangeService::processSettlements(std::size_t limit) {
    for (auto job : repository_.pendingSettlementJobs(limit)) {
        try {
            economy_.credit(job.player_name, job.amount, "settlement:" + std::to_string(job.id));
            job.done = true;
        } catch (...) {
            ++job.attempts;
        }
        repository_.updateSettlementJob(job);
    }
}

void ExchangeService::validateProduct(const std::string& product_key) const {
    const auto product = repository_.getProduct(product_key);
    if (!product || !product->tradable) {
        throw InvalidOrder("product is not tradable");
    }
}

void ExchangeService::validateQuantity(std::int32_t quantity) const {
    if (quantity <= 0) {
        throw InvalidOrder("quantity must be positive");
    }
}

void ExchangeService::validatePrice(std::int64_t unit_price) const {
    if (unit_price <= 0) {
        throw InvalidOrder("unit price must be positive");
    }
}

void ExchangeService::validateSnapshots(const std::string& product_key, const std::vector<ItemSnapshot>& snapshots) const {
    std::int32_t total = 0;
    for (const auto& snapshot : snapshots) {
        if (snapshot.product_key != product_key) {
            throw InvalidOrder("item snapshot does not match product");
        }
        validateQuantity(snapshot.quantity);
        total += snapshot.quantity;
    }
    validateQuantity(total);
}

void ExchangeService::matchBuyOrder(Order& buy_order) {
    const auto max_price = buy_order.type == OrderType::Limit ? buy_order.unit_price : std::nullopt;
    for (auto sell_order : repository_.matchingSellOrders(buy_order.product_key, max_price, 200)) {
        if (buy_order.remaining_quantity <= 0) {
            break;
        }
        const auto quantity = std::min({buy_order.remaining_quantity, sell_order.remaining_quantity, availableLotQuantity(sell_order.id)});
        if (quantity <= 0) {
            continue;
        }
        const auto price = *sell_order.unit_price;
        const auto trade = executeTrade(buy_order, sell_order, price, quantity);
        deliverLots(sell_order, buy_order, trade, quantity);
        consumeBuyReservation(buy_order, price, quantity, trade.id);
        sell_order.remaining_quantity -= quantity;
        sell_order.status = statusFor(sell_order);
        repository_.updateOrder(sell_order);
        buy_order.remaining_quantity -= quantity;
        buy_order.status = statusFor(buy_order);
        repository_.updateOrder(buy_order);
    }
    if (buy_order.remaining_quantity <= 0) {
        refundRemainingReservation(buy_order, "filled_remaining_refund");
        buy_order.status = OrderStatus::Filled;
        repository_.updateOrder(buy_order);
    }
}

void ExchangeService::matchSellOrder(Order& sell_order) {
    const auto min_price = sell_order.type == OrderType::Limit ? sell_order.unit_price : std::nullopt;
    for (auto buy_order : repository_.matchingBuyOrders(sell_order.product_key, min_price, 200)) {
        if (sell_order.remaining_quantity <= 0) {
            break;
        }
        const auto quantity = std::min({sell_order.remaining_quantity, buy_order.remaining_quantity, availableLotQuantity(sell_order.id)});
        if (quantity <= 0) {
            continue;
        }
        const auto price = *buy_order.unit_price;
        const auto trade = executeTrade(buy_order, sell_order, price, quantity);
        deliverLots(sell_order, buy_order, trade, quantity);
        consumeBuyReservation(buy_order, price, quantity, trade.id);
        buy_order.remaining_quantity -= quantity;
        buy_order.status = statusFor(buy_order);
        if (buy_order.remaining_quantity <= 0) {
            refundRemainingReservation(buy_order, "filled_remaining_refund");
            buy_order.status = OrderStatus::Filled;
        }
        repository_.updateOrder(buy_order);
        sell_order.remaining_quantity -= quantity;
        sell_order.status = statusFor(sell_order);
        repository_.updateOrder(sell_order);
    }
}

Trade ExchangeService::executeTrade(Order& buy_order, Order& sell_order, std::int64_t unit_price, std::int32_t quantity) {
    const auto total = unit_price * static_cast<std::int64_t>(quantity);
    auto trade = repository_.createTrade(Trade{
        0, buy_order.product_key, buy_order.id, sell_order.id, buy_order.player_uuid, buy_order.player_name,
        sell_order.player_uuid, sell_order.player_name, unit_price, quantity, total, sell_order.system_order});
    if (!sell_order.system_order) {
        repository_.createSettlementJob(SettlementJob{
            0, SettlementJobType::Credit, sell_order.player_uuid, sell_order.player_name, total, "trade_credit", trade.id});
    }
    return trade;
}

void ExchangeService::deliverLots(const Order& sell_order, const Order& buy_order, const Trade& trade, std::int32_t quantity) {
    auto remaining = quantity;
    for (auto lot : repository_.lotsForOrder(sell_order.id)) {
        if (remaining <= 0) {
            break;
        }
        const auto take = std::min(remaining, lot.remaining_quantity);
        lot.remaining_quantity -= take;
        repository_.updateItemLot(lot);
        repository_.addTradeItem(trade.id, lot.id, take, lot.nbt_blob, lot.nbt_summary);
        repository_.addMailboxItem(MailboxItem{
            0, buy_order.player_uuid, buy_order.player_name, buy_order.product_key, take,
            lot.nbt_blob, lot.nbt_summary, false});
        remaining -= take;
    }
}

void ExchangeService::consumeBuyReservation(Order& buy_order, std::int64_t execution_price, std::int32_t quantity, std::int64_t trade_id) {
    auto fund = repository_.reservedFundForOrder(buy_order.id);
    if (!fund) {
        return;
    }
    const auto reserved_unit = buy_order.unit_price.value_or(execution_price);
    const auto reserved_spent = reserved_unit * static_cast<std::int64_t>(quantity);
    const auto actual_spent = execution_price * static_cast<std::int64_t>(quantity);
    const auto refund = std::max<std::int64_t>(0, reserved_spent - actual_spent);
    fund->amount_remaining = std::max<std::int64_t>(0, fund->amount_remaining - reserved_spent);
    repository_.updateReservedFund(*fund);
    buy_order.reserved_amount = fund->amount_remaining;
    if (refund > 0) {
        repository_.createSettlementJob(SettlementJob{
            0, SettlementJobType::Refund, buy_order.player_uuid, buy_order.player_name, refund, "limit_price_improvement", trade_id});
    }
}

void ExchangeService::refundRemainingReservation(Order& order, const std::string& reason) {
    auto fund = repository_.reservedFundForOrder(order.id);
    if (!fund || fund->amount_remaining <= 0) {
        order.reserved_amount = 0;
        return;
    }
    repository_.createSettlementJob(SettlementJob{
        0, SettlementJobType::Refund, order.player_uuid, order.player_name, fund->amount_remaining, reason, std::nullopt});
    fund->amount_remaining = 0;
    repository_.updateReservedFund(*fund);
    order.reserved_amount = 0;
}

void ExchangeService::returnRemainingLots(Order& order, OrderStatus status) {
    for (auto lot : repository_.lotsForOrder(order.id)) {
        if (lot.remaining_quantity <= 0) {
            continue;
        }
        if (!order.system_order) {
            repository_.addMailboxItem(MailboxItem{
                0, order.player_uuid, order.player_name, order.product_key, lot.remaining_quantity,
                lot.nbt_blob, lot.nbt_summary, false});
        }
        lot.remaining_quantity = 0;
        repository_.updateItemLot(lot);
    }
    order.remaining_quantity = 0;
    order.status = status;
    repository_.updateOrder(order);
}

std::int32_t ExchangeService::availableLotQuantity(std::int64_t order_id) const {
    std::int32_t total = 0;
    for (const auto& lot : repository_.lotsForOrder(order_id)) {
        total += lot.remaining_quantity;
    }
    return total;
}

std::pair<std::int32_t, std::int64_t> ExchangeService::estimateMarketBuy(const std::string& product_key, std::int32_t quantity) const {
    std::int32_t filled = 0;
    std::int64_t total = 0;
    for (const auto& order : repository_.matchingSellOrders(product_key, std::nullopt, 500)) {
        if (filled >= quantity) {
            break;
        }
        const auto take = std::min(quantity - filled, order.remaining_quantity);
        filled += take;
        total += static_cast<std::int64_t>(take) * *order.unit_price;
    }
    return {filled, total};
}

std::pair<std::int32_t, std::int64_t> ExchangeService::estimateMarketSell(const std::string& product_key, std::int32_t quantity) const {
    std::int32_t filled = 0;
    std::int64_t total = 0;
    for (const auto& order : repository_.matchingBuyOrders(product_key, std::nullopt, 500)) {
        if (filled >= quantity) {
            break;
        }
        const auto take = std::min(quantity - filled, order.remaining_quantity);
        filled += take;
        total += static_cast<std::int64_t>(take) * *order.unit_price;
    }
    return {filled, total};
}

OrderStatus ExchangeService::statusFor(const Order& order) {
    if (order.remaining_quantity <= 0) {
        return OrderStatus::Filled;
    }
    if (order.remaining_quantity == order.original_quantity) {
        return OrderStatus::Open;
    }
    return OrderStatus::Partial;
}

}  // namespace exchange
