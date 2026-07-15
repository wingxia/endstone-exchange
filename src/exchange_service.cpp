#include "endstone_exchange/exchange_service.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <stdexcept>

namespace exchange {
namespace {

Cents checkedProduct(const Cents price, const int quantity)
{
    if (price < 0 || quantity < 0 || (quantity != 0 && price > std::numeric_limits<Cents>::max() / quantity)) {
        throw std::runtime_error("price multiplied by quantity is too large");
    }
    return price * quantity;
}

std::optional<std::int64_t> optionalInt64(const QueryRow &row, const std::size_t index)
{
    if (index >= row.size() || !row[index].has_value()) {
        return std::nullopt;
    }
    return cellInt64(row, index);
}

std::string orderStatus(const int remaining, const int filled)
{
    if (remaining == 0) {
        return "FILLED";
    }
    return filled > 0 ? "PARTIAL" : "OPEN";
}

std::vector<std::uint8_t> bytesFromCell(const QueryRow &row, const std::size_t index)
{
    const auto value = cellString(row, index);
    return {value.begin(), value.end()};
}

}  // namespace

ExchangeService::ExchangeService(Database &database, const Cents initial_balance_cents, const int max_order_quantity)
    : database_(database), initial_balance_cents_(initial_balance_cents), max_order_quantity_(max_order_quantity)
{
}

void ExchangeService::ensureAccount(const std::string_view player_uuid, const std::string_view player_name)
{
    if (player_uuid.empty()) {
        throw std::runtime_error("player UUID must not be empty");
    }
    database_.execute(std::format(
        "INSERT INTO exchange_accounts(player_uuid, player_name, balance_cents) VALUES ({}, {}, {}) "
        "ON DUPLICATE KEY UPDATE player_name=VALUES(player_name)",
        database_.quote(player_uuid), database_.quote(player_name), initial_balance_cents_));
}

Cents ExchangeService::balance(const std::string_view player_uuid)
{
    const auto rows = database_.query(std::format("SELECT balance_cents FROM exchange_accounts WHERE player_uuid={}",
                                                  database_.quote(player_uuid)));
    if (rows.empty()) {
        throw std::runtime_error("exchange account does not exist");
    }
    return cellInt64(rows.front(), 0);
}

Cents ExchangeService::addBalance(const std::string_view player_uuid, const std::string_view player_name,
                                  const Cents delta_cents)
{
    ensureAccount(player_uuid, player_name);
    Transaction transaction(database_);
    const auto current = lockedBalance(player_uuid);
    if (delta_cents < -current) {
        throw std::runtime_error("balance cannot become negative");
    }
    if (delta_cents > 0 && current > std::numeric_limits<Cents>::max() - delta_cents) {
        throw std::runtime_error("balance is too large");
    }
    database_.execute(std::format("UPDATE exchange_accounts SET balance_cents=balance_cents+{} WHERE player_uuid={}",
                                  delta_cents, database_.quote(player_uuid)));
    const auto result = current + delta_cents;
    transaction.commit();
    return result;
}

Market ExchangeService::activateMarket(const Market &market)
{
    if (market.target_key.empty() || market.dimension_name.empty() || market.item.type.empty() ||
        market.item.name.empty() || market.created_by.empty()) {
        throw std::runtime_error("market activation data is incomplete");
    }

    Transaction transaction(database_);
    const auto existing = database_.query(std::format("SELECT id FROM exchange_markets WHERE target_key={} FOR UPDATE",
                                                       database_.quote(market.target_key)));
    const auto block_x = market.block_x ? std::to_string(*market.block_x) : "NULL";
    const auto block_y = market.block_y ? std::to_string(*market.block_y) : "NULL";
    const auto block_z = market.block_z ? std::to_string(*market.block_z) : "NULL";
    const auto actor_id = market.actor_id ? std::to_string(*market.actor_id) : "NULL";
    Id id = 0;
    if (existing.empty()) {
        database_.execute(std::format(
            "INSERT INTO exchange_markets(target_key,target_kind,dimension_name,block_x,block_y,block_z,actor_id,"
            "item_type,item_data,item_nbt,item_name,active,created_by) VALUES ({},{},{},{},{},{},{},{},{},{},{},1,{})",
            database_.quote(market.target_key), database_.quote(toSql(market.target_kind)),
            database_.quote(market.dimension_name), block_x, block_y, block_z, actor_id,
            database_.quote(market.item.type), market.item.data, Database::hexLiteral(market.item.nbt),
            database_.quote(market.item.name), database_.quote(market.created_by)));
        id = database_.lastInsertId();
    }
    else {
        id = static_cast<Id>(cellInt64(existing.front(), 0));
        database_.execute(std::format(
            "UPDATE exchange_markets SET target_kind={},dimension_name={},block_x={},block_y={},block_z={},actor_id={},"
            "item_type={},item_data={},item_nbt={},item_name={},active=1,created_by={} WHERE id={}",
            database_.quote(toSql(market.target_kind)), database_.quote(market.dimension_name), block_x, block_y,
            block_z, actor_id, database_.quote(market.item.type), market.item.data,
            Database::hexLiteral(market.item.nbt), database_.quote(market.item.name),
            database_.quote(market.created_by), id));
    }
    transaction.commit();
    const auto activated = findMarket(id);
    if (!activated) {
        throw std::runtime_error("activated market could not be reloaded");
    }
    return *activated;
}

void ExchangeService::deactivateMarket(const Id market_id)
{
    Transaction transaction(database_);
    const auto market = database_.query(
        std::format("SELECT active FROM exchange_markets WHERE id={} FOR UPDATE", market_id));
    if (market.empty()) {
        throw std::runtime_error("market does not exist");
    }
    if (cellInt(market.front(), 0) == 0) {
        transaction.commit();
        return;
    }

    const auto orders = database_.query(std::format(
        "SELECT id,player_uuid,side,remaining_qty,reserved_cents FROM exchange_orders "
        "WHERE market_id={} AND order_type='LIMIT' AND status IN ('OPEN','PARTIAL') FOR UPDATE",
        market_id));
    for (const auto &order : orders) {
        const auto order_id = static_cast<Id>(cellInt64(order, 0));
        const auto owner = cellString(order, 1);
        const auto side = cellString(order, 2);
        const auto remaining = cellInt(order, 3);
        const auto reserved = cellInt64(order, 4);
        if (side == "BUY") {
            credit(owner, reserved);
        }
        else if (remaining > 0) {
            addDelivery(owner, market_id, remaining, "MARKET_CLOSED");
        }
        database_.execute(std::format(
            "UPDATE exchange_orders SET remaining_qty=0,reserved_cents=0,status='CANCELED' WHERE id={}", order_id));
    }
    database_.execute(std::format("UPDATE exchange_markets SET active=0 WHERE id={}", market_id));
    transaction.commit();
}

std::optional<Market> ExchangeService::findMarketByTarget(const std::string_view target_key)
{
    const auto rows = database_.query(std::format(
        "SELECT id,target_key,target_kind,dimension_name,block_x,block_y,block_z,actor_id,item_type,item_data,item_nbt,"
        "item_name,active,created_by FROM exchange_markets WHERE target_key={} AND active=1",
        database_.quote(target_key)));
    return rows.empty() ? std::nullopt : std::optional<Market>(marketFromRow(rows.front()));
}

std::optional<Market> ExchangeService::findMarket(const Id market_id)
{
    const auto rows = database_.query(std::format(
        "SELECT id,target_key,target_kind,dimension_name,block_x,block_y,block_z,actor_id,item_type,item_data,item_nbt,"
        "item_name,active,created_by FROM exchange_markets WHERE id={}",
        market_id));
    return rows.empty() ? std::nullopt : std::optional<Market>(marketFromRow(rows.front()));
}

std::vector<Market> ExchangeService::activeMarkets()
{
    const auto rows = database_.query(
        "SELECT id,target_key,target_kind,dimension_name,block_x,block_y,block_z,actor_id,item_type,item_data,item_nbt,"
        "item_name,active,created_by FROM exchange_markets WHERE active=1 ORDER BY id");
    std::vector<Market> result;
    result.reserve(rows.size());
    for (const auto &row : rows) {
        result.push_back(marketFromRow(row));
    }
    return result;
}

OrderBook ExchangeService::orderBook(const Id market_id, const int depth)
{
    if (depth <= 0 || depth > 50) {
        throw std::runtime_error("order-book depth must be between 1 and 50");
    }
    OrderBook result;
    const auto bids = database_.query(std::format(
        "SELECT price_cents,SUM(remaining_qty) FROM exchange_orders WHERE market_id={} AND side='BUY' "
        "AND order_type='LIMIT' AND status IN ('OPEN','PARTIAL') GROUP BY price_cents "
        "ORDER BY price_cents DESC LIMIT {}",
        market_id, depth));
    for (const auto &row : bids) {
        result.bids.push_back({cellInt64(row, 0), cellInt(row, 1)});
    }
    const auto asks = database_.query(std::format(
        "SELECT price_cents,SUM(remaining_qty) FROM exchange_orders WHERE market_id={} AND side='SELL' "
        "AND order_type='LIMIT' AND status IN ('OPEN','PARTIAL') GROUP BY price_cents "
        "ORDER BY price_cents ASC LIMIT {}",
        market_id, depth));
    for (const auto &row : asks) {
        result.asks.push_back({cellInt64(row, 0), cellInt(row, 1)});
    }
    const auto last = database_.query(std::format(
        "SELECT price_cents FROM exchange_trades WHERE market_id={} ORDER BY id DESC LIMIT 1", market_id));
    if (!last.empty()) {
        result.last_price_cents = cellInt64(last.front(), 0);
    }
    return result;
}

ExecutionResult ExchangeService::placeOrder(const OrderRequest &request)
{
    validateRequest(request);
    ensureAccount(request.player_uuid, request.player_name);
    return request.side == Side::Buy ? placeBuy(request) : placeSell(request);
}

ExecutionResult ExchangeService::placeBuy(const OrderRequest &request)
{
    Transaction transaction(database_);
    const auto market = database_.query(
        std::format("SELECT active FROM exchange_markets WHERE id={} FOR UPDATE", request.market_id));
    if (market.empty() || cellInt(market.front(), 0) == 0) {
        throw std::runtime_error("market is not active");
    }

    auto available_balance = lockedBalance(request.player_uuid);
    Cents reserved = 0;
    if (request.type == OrderType::Limit) {
        reserved = checkedProduct(request.price_cents, request.quantity);
        if (available_balance < reserved) {
            throw std::runtime_error("insufficient exchange balance");
        }
        debit(request.player_uuid, reserved);
        available_balance -= reserved;
    }

    database_.execute(std::format(
        "INSERT INTO exchange_orders(market_id,player_uuid,side,order_type,price_cents,original_qty,remaining_qty,"
        "reserved_cents,status) VALUES ({},{},'BUY',{}, {},{},{},{},'OPEN')",
        request.market_id, database_.quote(request.player_uuid), database_.quote(toSql(request.type)),
        request.type == OrderType::Limit ? request.price_cents : 0, request.quantity, request.quantity, reserved));
    const auto order_id = static_cast<Id>(database_.lastInsertId());

    const auto price_filter = request.type == OrderType::Limit
                                  ? std::format("AND price_cents<={}", request.price_cents)
                                  : std::string{};
    const auto asks = database_.query(std::format(
        "SELECT id,player_uuid,price_cents,remaining_qty FROM exchange_orders WHERE market_id={} AND side='SELL' "
        "AND order_type='LIMIT' AND status IN ('OPEN','PARTIAL') AND player_uuid<>{} {} "
        "ORDER BY price_cents ASC,id ASC FOR UPDATE",
        request.market_id, database_.quote(request.player_uuid), price_filter));

    int remaining = request.quantity;
    int filled_total = 0;
    Cents gross = 0;
    for (const auto &ask : asks) {
        if (remaining == 0) {
            break;
        }
        const auto ask_id = static_cast<Id>(cellInt64(ask, 0));
        const auto seller = cellString(ask, 1);
        const auto price = cellInt64(ask, 2);
        const auto ask_remaining = cellInt(ask, 3);
        int fill = std::min(remaining, ask_remaining);
        if (request.type == OrderType::Market) {
            fill = std::min<std::int64_t>(fill, available_balance / price);
            if (fill <= 0) {
                break;
            }
            const auto cost = checkedProduct(price, fill);
            debit(request.player_uuid, cost);
            available_balance -= cost;
        }
        else {
            const auto refund = checkedProduct(request.price_cents - price, fill);
            if (refund > 0) {
                credit(request.player_uuid, refund);
                available_balance += refund;
            }
        }

        const auto ask_after = ask_remaining - fill;
        database_.execute(std::format(
            "UPDATE exchange_orders SET remaining_qty={},status='{}' WHERE id={}", ask_after,
            ask_after == 0 ? "FILLED" : "PARTIAL", ask_id));
        const auto cost = checkedProduct(price, fill);
        credit(seller, cost);
        addDelivery(request.player_uuid, request.market_id, fill, "TRADE_BUY");
        database_.execute(std::format(
            "INSERT INTO exchange_trades(market_id,buy_order_id,sell_order_id,buyer_uuid,seller_uuid,price_cents,"
            "quantity) VALUES ({},{},{},{},{},{},{})",
            request.market_id, order_id, ask_id, database_.quote(request.player_uuid), database_.quote(seller), price,
            fill));
        remaining -= fill;
        filled_total += fill;
        gross += cost;
    }

    const auto current_reserved =
        request.type == OrderType::Limit ? checkedProduct(request.price_cents, remaining) : 0;
    const auto status = request.type == OrderType::Market
                            ? (filled_total == request.quantity ? "FILLED" : (filled_total > 0 ? "PARTIAL" : "CANCELED"))
                            : orderStatus(remaining, filled_total);
    const auto persisted_remaining = request.type == OrderType::Limit ? remaining : 0;
    database_.execute(std::format(
        "UPDATE exchange_orders SET remaining_qty={},reserved_cents={},status='{}' WHERE id={}", persisted_remaining,
        current_reserved, status, order_id));
    const auto final_balance = lockedBalance(request.player_uuid);
    transaction.commit();
    return {order_id, request.quantity, filled_total,
            request.type == OrderType::Limit ? remaining : 0, gross, final_balance};
}

ExecutionResult ExchangeService::placeSell(const OrderRequest &request)
{
    Transaction transaction(database_);
    const auto market = database_.query(
        std::format("SELECT active FROM exchange_markets WHERE id={} FOR UPDATE", request.market_id));
    if (market.empty() || cellInt(market.front(), 0) == 0) {
        throw std::runtime_error("market is not active");
    }
    static_cast<void>(lockedBalance(request.player_uuid));

    database_.execute(std::format(
        "INSERT INTO exchange_orders(market_id,player_uuid,side,order_type,price_cents,original_qty,remaining_qty,"
        "reserved_cents,status) VALUES ({},{},'SELL',{}, {},{},{},0,'OPEN')",
        request.market_id, database_.quote(request.player_uuid), database_.quote(toSql(request.type)),
        request.type == OrderType::Limit ? request.price_cents : 0, request.quantity, request.quantity));
    const auto order_id = static_cast<Id>(database_.lastInsertId());

    const auto price_filter = request.type == OrderType::Limit
                                  ? std::format("AND price_cents>={}", request.price_cents)
                                  : std::string{};
    const auto bids = database_.query(std::format(
        "SELECT id,player_uuid,price_cents,remaining_qty,reserved_cents FROM exchange_orders WHERE market_id={} "
        "AND side='BUY' AND order_type='LIMIT' AND status IN ('OPEN','PARTIAL') AND player_uuid<>{} {} "
        "ORDER BY price_cents DESC,id ASC FOR UPDATE",
        request.market_id, database_.quote(request.player_uuid), price_filter));

    int remaining = request.quantity;
    int filled_total = 0;
    Cents gross = 0;
    for (const auto &bid : bids) {
        if (remaining == 0) {
            break;
        }
        const auto bid_id = static_cast<Id>(cellInt64(bid, 0));
        const auto buyer = cellString(bid, 1);
        const auto price = cellInt64(bid, 2);
        const auto bid_remaining = cellInt(bid, 3);
        const auto bid_reserved = cellInt64(bid, 4);
        const auto fill = std::min(remaining, bid_remaining);
        const auto bid_after = bid_remaining - fill;
        const auto reserved_after = bid_reserved - checkedProduct(price, fill);
        if (reserved_after < 0) {
            throw std::runtime_error("buy-order reserve invariant failed");
        }
        database_.execute(std::format(
            "UPDATE exchange_orders SET remaining_qty={},reserved_cents={},status='{}' WHERE id={}", bid_after,
            reserved_after, bid_after == 0 ? "FILLED" : "PARTIAL", bid_id));
        const auto proceeds = checkedProduct(price, fill);
        credit(request.player_uuid, proceeds);
        addDelivery(buyer, request.market_id, fill, "TRADE_BUY");
        database_.execute(std::format(
            "INSERT INTO exchange_trades(market_id,buy_order_id,sell_order_id,buyer_uuid,seller_uuid,price_cents,"
            "quantity) VALUES ({},{},{},{},{},{},{})",
            request.market_id, bid_id, order_id, database_.quote(buyer), database_.quote(request.player_uuid), price,
            fill));
        remaining -= fill;
        filled_total += fill;
        gross += proceeds;
    }

    if (request.type == OrderType::Market && remaining > 0) {
        addDelivery(request.player_uuid, request.market_id, remaining, "MARKET_REMAINDER");
    }
    const auto status = request.type == OrderType::Market
                            ? (filled_total == request.quantity ? "FILLED" : (filled_total > 0 ? "PARTIAL" : "CANCELED"))
                            : orderStatus(remaining, filled_total);
    const auto persisted_remaining = request.type == OrderType::Limit ? remaining : 0;
    database_.execute(std::format("UPDATE exchange_orders SET remaining_qty={},status='{}' WHERE id={}",
                                  persisted_remaining, status, order_id));
    const auto final_balance = lockedBalance(request.player_uuid);
    transaction.commit();
    return {order_id, request.quantity, filled_total,
            request.type == OrderType::Limit ? remaining : 0, gross, final_balance};
}

void ExchangeService::cancelOrder(const Id order_id, const std::string_view player_uuid, const bool administrator)
{
    Transaction transaction(database_);
    const auto rows = database_.query(std::format(
        "SELECT market_id,player_uuid,side,order_type,remaining_qty,reserved_cents,status FROM exchange_orders "
        "WHERE id={} FOR UPDATE",
        order_id));
    if (rows.empty()) {
        throw std::runtime_error("order does not exist");
    }
    const auto &row = rows.front();
    const auto market_id = static_cast<Id>(cellInt64(row, 0));
    const auto owner = cellString(row, 1);
    const auto side = cellString(row, 2);
    const auto type = cellString(row, 3);
    const auto remaining = cellInt(row, 4);
    const auto reserved = cellInt64(row, 5);
    const auto status = cellString(row, 6);
    if (!administrator && owner != player_uuid) {
        throw std::runtime_error("cannot cancel another player's order");
    }
    if (type != "LIMIT" || (status != "OPEN" && status != "PARTIAL")) {
        throw std::runtime_error("order is not open");
    }
    if (side == "BUY") {
        credit(owner, reserved);
    }
    else if (remaining > 0) {
        addDelivery(owner, market_id, remaining, "ORDER_CANCELED");
    }
    database_.execute(std::format(
        "UPDATE exchange_orders SET remaining_qty=0,reserved_cents=0,status='CANCELED' WHERE id={}", order_id));
    transaction.commit();
}

std::vector<OpenOrder> ExchangeService::openOrders(const std::string_view player_uuid)
{
    const auto rows = database_.query(std::format(
        "SELECT o.id,o.market_id,m.item_name,o.side,o.price_cents,o.remaining_qty FROM exchange_orders o "
        "JOIN exchange_markets m ON m.id=o.market_id WHERE o.player_uuid={} AND o.order_type='LIMIT' "
        "AND o.status IN ('OPEN','PARTIAL') ORDER BY o.id DESC",
        database_.quote(player_uuid)));
    std::vector<OpenOrder> result;
    result.reserve(rows.size());
    for (const auto &row : rows) {
        result.push_back({static_cast<Id>(cellInt64(row, 0)), static_cast<Id>(cellInt64(row, 1)), cellString(row, 2),
                          cellString(row, 3) == "BUY" ? Side::Buy : Side::Sell, cellInt64(row, 4), cellInt(row, 5)});
    }
    return result;
}

std::vector<Delivery> ExchangeService::pendingDeliveries(const std::string_view player_uuid)
{
    const auto rows = database_.query(std::format(
        "SELECT d.id,d.market_id,m.item_type,m.item_data,m.item_nbt,m.item_name,d.quantity,d.claimed_qty,d.reason "
        "FROM exchange_deliveries d JOIN exchange_markets m ON m.id=d.market_id WHERE d.player_uuid={} "
        "AND d.claimed_qty<d.quantity ORDER BY d.id",
        database_.quote(player_uuid)));
    std::vector<Delivery> result;
    result.reserve(rows.size());
    for (const auto &row : rows) {
        result.push_back({static_cast<Id>(cellInt64(row, 0)), static_cast<Id>(cellInt64(row, 1)),
                          {cellString(row, 2), cellInt(row, 3), bytesFromCell(row, 4), cellString(row, 5)},
                          cellInt(row, 6), cellInt(row, 7), cellString(row, 8)});
    }
    return result;
}

void ExchangeService::markDeliveryClaimed(const Id delivery_id, const int quantity)
{
    if (quantity <= 0) {
        throw std::runtime_error("claimed quantity must be positive");
    }
    Transaction transaction(database_);
    const auto rows = database_.query(std::format(
        "SELECT quantity,claimed_qty FROM exchange_deliveries WHERE id={} FOR UPDATE", delivery_id));
    if (rows.empty()) {
        throw std::runtime_error("delivery does not exist");
    }
    const auto total = cellInt(rows.front(), 0);
    const auto claimed = cellInt(rows.front(), 1);
    if (quantity > total - claimed) {
        throw std::runtime_error("claimed quantity exceeds pending delivery");
    }
    database_.execute(std::format("UPDATE exchange_deliveries SET claimed_qty=claimed_qty+{} WHERE id={}", quantity,
                                  delivery_id));
    transaction.commit();
}

Market ExchangeService::marketFromRow(const QueryRow &row)
{
    Market result;
    result.id = static_cast<Id>(cellInt64(row, 0));
    result.target_key = cellString(row, 1);
    result.target_kind = cellString(row, 2) == "BLOCK" ? TargetKind::Block : TargetKind::Actor;
    result.dimension_name = cellString(row, 3);
    if (const auto value = optionalInt64(row, 4)) {
        result.block_x = static_cast<int>(*value);
    }
    if (const auto value = optionalInt64(row, 5)) {
        result.block_y = static_cast<int>(*value);
    }
    if (const auto value = optionalInt64(row, 6)) {
        result.block_z = static_cast<int>(*value);
    }
    result.actor_id = optionalInt64(row, 7);
    result.item = {cellString(row, 8), cellInt(row, 9), bytesFromCell(row, 10), cellString(row, 11)};
    result.active = cellInt(row, 12) != 0;
    result.created_by = cellString(row, 13);
    return result;
}

Cents ExchangeService::lockedBalance(const std::string_view player_uuid)
{
    const auto rows = database_.query(std::format(
        "SELECT balance_cents FROM exchange_accounts WHERE player_uuid={} FOR UPDATE", database_.quote(player_uuid)));
    if (rows.empty()) {
        throw std::runtime_error("exchange account does not exist");
    }
    return cellInt64(rows.front(), 0);
}

void ExchangeService::credit(const std::string_view player_uuid, const Cents amount)
{
    if (amount < 0) {
        throw std::runtime_error("cannot credit a negative amount");
    }
    if (amount == 0) {
        return;
    }
    database_.execute(std::format("UPDATE exchange_accounts SET balance_cents=balance_cents+{} WHERE player_uuid={}",
                                  amount, database_.quote(player_uuid)));
    if (database_.affectedRows() != 1) {
        throw std::runtime_error("credit target account does not exist");
    }
}

void ExchangeService::debit(const std::string_view player_uuid, const Cents amount)
{
    if (amount < 0) {
        throw std::runtime_error("cannot debit a negative amount");
    }
    if (amount == 0) {
        return;
    }
    database_.execute(std::format(
        "UPDATE exchange_accounts SET balance_cents=balance_cents-{} WHERE player_uuid={} AND balance_cents>={}",
        amount, database_.quote(player_uuid), amount));
    if (database_.affectedRows() != 1) {
        throw std::runtime_error("insufficient exchange balance");
    }
}

void ExchangeService::addDelivery(const std::string_view player_uuid, const Id market_id, const int quantity,
                                  const std::string_view reason)
{
    if (quantity <= 0) {
        return;
    }
    database_.execute(std::format(
        "INSERT INTO exchange_deliveries(player_uuid,market_id,quantity,reason) VALUES ({},{},{},{})",
        database_.quote(player_uuid), market_id, quantity, database_.quote(reason)));
}

void ExchangeService::validateRequest(const OrderRequest &request)
{
    if (request.market_id == 0 || request.player_uuid.empty() || request.player_name.empty()) {
        throw std::runtime_error("order identity is incomplete");
    }
    if (request.quantity <= 0 || request.quantity > max_order_quantity_) {
        throw std::runtime_error("order quantity is outside the configured range");
    }
    if (request.type == OrderType::Limit && request.price_cents <= 0) {
        throw std::runtime_error("limit-order price must be positive");
    }
    if (request.type == OrderType::Limit) {
        static_cast<void>(checkedProduct(request.price_cents, request.quantity));
    }
}

}  // namespace exchange
