#include "endstone_exchange/exchange_service.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <stdexcept>

namespace exchange {
namespace {

Cents checkedProduct(const Cents price, const int quantity) {
    if (price < 0 || quantity < 0 || (quantity != 0 && price > std::numeric_limits<Cents>::max() / quantity)) {
        throw std::runtime_error("price multiplied by quantity is too large");
    }
    return price * quantity;
}

std::optional<std::int64_t> optionalInt64(const QueryRow &row, const std::size_t index) {
    if (index >= row.size() || !row[index].has_value()) {
        return std::nullopt;
    }
    return cellInt64(row, index);
}

std::optional<std::string> optionalString(const QueryRow &row, const std::size_t index) {
    if (index >= row.size() || !row[index].has_value()) {
        return std::nullopt;
    }
    return cellString(row, index);
}

std::string orderStatus(const int remaining, const int filled) {
    if (remaining == 0) {
        return "FILLED";
    }
    return filled > 0 ? "PARTIAL" : "OPEN";
}

std::vector<std::uint8_t> bytesFromCell(const QueryRow &row, const std::size_t index) {
    const auto value = cellString(row, index);
    return {value.begin(), value.end()};
}

std::string idList(const std::vector<Id> &ids) {
    if (ids.empty()) {
        throw std::runtime_error("market id list must not be empty");
    }
    std::string result;
    for (const auto id : ids) {
        if (id == 0) {
            throw std::runtime_error("market id must be positive");
        }
        if (!result.empty()) {
            result.push_back(',');
        }
        result += std::to_string(id);
    }
    return result;
}

DeliveryClaimStatus deliveryClaimStatus(const std::string_view status) {
    if (status == "PREPARED") {
        return DeliveryClaimStatus::Prepared;
    }
    if (status == "APPLIED") {
        return DeliveryClaimStatus::Applied;
    }
    if (status == "CANCELED") {
        return DeliveryClaimStatus::Canceled;
    }
    throw std::runtime_error("delivery claim has an unknown status");
}

DeliveryClaim deliveryClaimFromRow(const QueryRow &row) {
    return {static_cast<Id>(cellInt64(row, 0)),
            static_cast<Id>(cellInt64(row, 1)),
            {cellString(row, 2), cellInt(row, 3), bytesFromCell(row, 4), cellString(row, 5)},
            cellInt(row, 6),
            cellInt(row, 7),
            deliveryClaimStatus(cellString(row, 8)),
            cellInt(row, 9) != 0};
}

SellEscrowStatus sellEscrowStatus(const std::string_view status) {
    if (status == "PREPARED") {
        return SellEscrowStatus::Prepared;
    }
    if (status == "TAGGED") {
        return SellEscrowStatus::Tagged;
    }
    if (status == "ORDERED") {
        return SellEscrowStatus::Ordered;
    }
    if (status == "CANCELED") {
        return SellEscrowStatus::Canceled;
    }
    throw std::runtime_error("sell escrow has an unknown status");
}

SellEscrow sellEscrowFromRow(const QueryRow &row) {
    std::optional<Id> order_id;
    if (const auto value = optionalInt64(row, 14)) {
        order_id = static_cast<Id>(*value);
    }
    return {static_cast<Id>(cellInt64(row, 0)),
            static_cast<Id>(cellInt64(row, 1)),
            {cellString(row, 2), cellInt(row, 3), bytesFromCell(row, 4), cellString(row, 5)},
            cellString(row, 6),
            cellString(row, 7),
            cellString(row, 8) == "LIMIT" ? OrderType::Limit : OrderType::Market,
            cellInt64(row, 9),
            cellInt(row, 10),
            cellInt(row, 11),
            cellInt(row, 12),
            sellEscrowStatus(cellString(row, 13)),
            order_id,
            cellInt(row, 15) != 0};
}

EconomyTransferStatus economyTransferStatus(const std::string_view status) {
    if (status == "PREPARED") {
        return EconomyTransferStatus::Prepared;
    }
    if (status == "EXTERNAL_APPLIED") {
        return EconomyTransferStatus::ExternalApplied;
    }
    if (status == "COMPLETED") {
        return EconomyTransferStatus::Completed;
    }
    if (status == "FAILED") {
        return EconomyTransferStatus::Failed;
    }
    if (status == "BLOCKED") {
        return EconomyTransferStatus::Blocked;
    }
    throw std::runtime_error("economy transfer has an unknown status");
}

FrameListingStatus frameListingStatus(const std::string_view status) {
    if (status == "ACTIVE") {
        return FrameListingStatus::Active;
    }
    if (status == "PAID") {
        return FrameListingStatus::Paid;
    }
    if (status == "DROPPED") {
        return FrameListingStatus::Dropped;
    }
    if (status == "CLAIMED") {
        return FrameListingStatus::Claimed;
    }
    if (status == "CANCELED") {
        return FrameListingStatus::Canceled;
    }
    throw std::runtime_error("frame listing has an unknown status");
}

constexpr std::string_view FrameListingColumns =
    "l.id,l.target_key,l.dimension_name,l.block_x,l.block_y,l.block_z,"
    "l.seller_uuid,l.seller_name,l.price_cents,l.item_type,l.item_data,l.item_nbt,l.item_name,"
    "l.status,l.buyer_uuid,l.buyer_name";

} // namespace

ExchangeService::ExchangeService(Database &database, const Cents initial_balance_cents, const int max_order_quantity)
    : database_(database), initial_balance_cents_(initial_balance_cents), max_order_quantity_(max_order_quantity) {}

void ExchangeService::ensureAccount(const std::string_view player_uuid, const std::string_view player_name) {
    if (player_uuid.empty()) {
        throw std::runtime_error("player UUID must not be empty");
    }
    Transaction transaction(database_);
    database_.execute(
        std::format("INSERT IGNORE INTO exchange_accounts(player_uuid,player_name,balance_cents) VALUES ({},{},{})",
                    database_.quote(player_uuid), database_.quote(player_name), initial_balance_cents_));
    const bool created = database_.affectedRows() == 1;
    if (created) {
        recordBalanceChange(player_uuid, initial_balance_cents_, "ACCOUNT_OPENED", {}, std::nullopt);
    } else {
        database_.execute(std::format("UPDATE exchange_accounts SET player_name={} WHERE player_uuid={}",
                                      database_.quote(player_name), database_.quote(player_uuid)));
    }
    transaction.commit();
}

Cents ExchangeService::balance(const std::string_view player_uuid) {
    const auto rows = database_.query(
        std::format("SELECT balance_cents FROM exchange_accounts WHERE player_uuid={}", database_.quote(player_uuid)));
    if (rows.empty()) {
        throw std::runtime_error("exchange account does not exist");
    }
    return cellInt64(rows.front(), 0);
}

Cents ExchangeService::addBalance(const std::string_view player_uuid, const std::string_view player_name,
                                  const Cents delta_cents) {
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
    recordBalanceChange(player_uuid, delta_cents, "ADMIN_ADJUSTMENT", {}, std::nullopt);
    const auto result = current + delta_cents;
    transaction.commit();
    return result;
}

Market ExchangeService::activateMarket(const Market &market) {
    if (market.target_key.empty() || market.dimension_name.empty() || market.item.type.empty() ||
        market.item.name.empty() || market.created_by.empty()) {
        throw std::runtime_error("market activation data is incomplete");
    }

    Transaction transaction(database_);
    const auto existing =
        database_.query(std::format("SELECT market_id FROM exchange_target_bindings WHERE target_key={} FOR UPDATE",
                                    database_.quote(market.target_key)));
    if (!existing.empty()) {
        throw std::runtime_error("target already has an active market");
    }

    const auto item_type = database_.quote(market.item.type);
    const auto item_nbt = Database::hexLiteral(market.item.nbt);
    const auto item_hash = std::format(
        "UNHEX(SHA2(CONCAT(CHAR_LENGTH({0}),':',{0},':',{1},':',OCTET_LENGTH({2}),':',{2}),256))", item_type,
        market.item.data, item_nbt);
    database_.execute(std::format(
        "INSERT INTO exchange_books(item_type,item_data,item_nbt,item_name,item_hash) VALUES ({},{},{},{},{}) "
        "ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id)",
        item_type, market.item.data, item_nbt, database_.quote(market.item.name), item_hash));
    const auto book_id = static_cast<Id>(database_.lastInsertId());
    const auto book = database_.query(std::format(
        "SELECT id FROM exchange_books WHERE id={} AND item_type={} AND item_data={} AND item_nbt={} FOR UPDATE",
        book_id, item_type, market.item.data, item_nbt));
    if (book.empty()) {
        throw std::runtime_error("item order-book fingerprint collision");
    }

    const auto resumable = database_.query(std::format(
        "SELECT id FROM exchange_markets WHERE target_key={} AND target_kind={} AND active=0 AND reopenable=1 "
        "AND book_id={} ORDER BY id DESC LIMIT 1 FOR UPDATE",
        database_.quote(market.target_key), database_.quote(toSql(market.target_kind)), book_id));
    if (!resumable.empty()) {
        const auto id = static_cast<Id>(cellInt64(resumable.front(), 0));
        database_.execute(
            std::format("UPDATE exchange_markets SET active=1,reopenable=0 WHERE id={}", id));
        database_.execute(std::format("INSERT INTO exchange_target_bindings(target_key,market_id) VALUES ({},{})",
                                      database_.quote(market.target_key), id));
        transaction.commit();
        const auto activated = findMarket(id);
        if (!activated) {
            throw std::runtime_error("resumed market could not be reloaded");
        }
        return *activated;
    }

    const auto block_x = market.block_x ? std::to_string(*market.block_x) : "NULL";
    const auto block_y = market.block_y ? std::to_string(*market.block_y) : "NULL";
    const auto block_z = market.block_z ? std::to_string(*market.block_z) : "NULL";
    const auto actor_id = market.actor_id ? std::to_string(*market.actor_id) : "NULL";
    database_.execute(std::format(
        "INSERT INTO exchange_markets(book_id,target_key,target_kind,dimension_name,block_x,block_y,block_z,actor_id,"
        "item_type,item_data,item_nbt,item_name,active,created_by) VALUES ({},{},{},{},{},{},{},{},{},{},{},{},1,{})",
        book_id, database_.quote(market.target_key), database_.quote(toSql(market.target_kind)),
        database_.quote(market.dimension_name), block_x, block_y, block_z, actor_id, item_type, market.item.data, item_nbt,
        database_.quote(market.item.name), database_.quote(market.created_by)));
    const auto id = static_cast<Id>(database_.lastInsertId());
    database_.execute(std::format("INSERT INTO exchange_target_bindings(target_key,market_id) VALUES ({},{})",
                                  database_.quote(market.target_key), id));
    transaction.commit();
    const auto activated = findMarket(id);
    if (!activated) {
        throw std::runtime_error("activated market could not be reloaded");
    }
    return *activated;
}

void ExchangeService::closeMarket(const Id market_id) {
    Transaction transaction(database_);
    const auto market = database_.query(
        std::format("SELECT active,target_key FROM exchange_markets WHERE id={} FOR UPDATE", market_id));
    if (market.empty()) {
        throw std::runtime_error("market does not exist");
    }
    if (cellInt(market.front(), 0) == 0) {
        database_.execute(std::format("DELETE FROM exchange_target_bindings WHERE market_id={}", market_id));
        transaction.commit();
        return;
    }

    database_.execute(std::format("DELETE FROM exchange_target_bindings WHERE target_key={} AND market_id={}",
                                  database_.quote(cellString(market.front(), 1)), market_id));
    if (database_.affectedRows() != 1) {
        throw std::runtime_error("active market target binding is missing");
    }
    database_.execute(
        std::format("UPDATE exchange_markets SET active=0,reopenable=1 WHERE id={}", market_id));
    transaction.commit();
}

void ExchangeService::retireMarket(const Id market_id) {
    Transaction transaction(database_);
    const auto identity =
        database_.query(std::format("SELECT book_id FROM exchange_markets WHERE id={}", market_id));
    if (identity.empty()) {
        throw std::runtime_error("market does not exist");
    }
    const auto book_id = static_cast<Id>(cellInt64(identity.front(), 0));
    if (database_.query(std::format("SELECT id FROM exchange_books WHERE id={} FOR UPDATE", book_id)).empty()) {
        throw std::runtime_error("market order book does not exist");
    }
    const auto market = database_.query(
        std::format("SELECT active,target_key,book_id FROM exchange_markets WHERE id={} FOR UPDATE", market_id));
    if (market.empty()) {
        throw std::runtime_error("market does not exist");
    }
    if (static_cast<Id>(cellInt64(market.front(), 2)) != book_id) {
        throw std::runtime_error("market order-book identity changed unexpectedly");
    }

    const auto orders = database_.query(
        std::format("SELECT id,player_uuid,side,remaining_qty,reserved_cents FROM exchange_orders "
                    "WHERE market_id={} AND order_type='LIMIT' AND status IN ('OPEN','PARTIAL') FOR UPDATE",
                    market_id));
    for (const auto &order : orders) {
        const auto order_id = static_cast<Id>(cellInt64(order, 0));
        const auto owner = cellString(order, 1);
        const auto side = cellString(order, 2);
        const auto remaining = cellInt(order, 3);
        const auto reserved = cellInt64(order, 4);
        if (side == "BUY") {
            credit(owner, reserved, "MARKET_CLOSE_REFUND", "ORDER", order_id);
        } else if (remaining > 0) {
            addDelivery(owner, market_id, remaining, "MARKET_CLOSED");
        }
        database_.execute(std::format(
            "UPDATE exchange_orders SET remaining_qty=0,reserved_cents=0,status='CANCELED' WHERE id={}", order_id));
    }
    database_.execute(std::format("DELETE FROM exchange_target_bindings WHERE target_key={} AND market_id={}",
                                  database_.quote(cellString(market.front(), 1)), market_id));
    if (cellInt(market.front(), 0) != 0 && database_.affectedRows() != 1) {
        throw std::runtime_error("active market target binding is missing");
    }
    database_.execute(
        std::format("UPDATE exchange_markets SET active=0,reopenable=0 WHERE id={}", market_id));
    transaction.commit();
}

std::optional<Market> ExchangeService::findMarketByTarget(const std::string_view target_key) {
    const auto rows = database_.query(std::format(
        "SELECT m.id,m.book_id,m.target_key,m.target_kind,m.dimension_name,m.block_x,m.block_y,m.block_z,m.actor_id,"
        "m.item_type,m.item_data,m.item_nbt,m.item_name,m.active,m.created_by FROM exchange_target_bindings b "
        "JOIN exchange_markets m ON m.id=b.market_id WHERE b.target_key={} AND m.active=1",
        database_.quote(target_key)));
    return rows.empty() ? std::nullopt : std::optional<Market>(marketFromRow(rows.front()));
}

std::optional<Market> ExchangeService::findMarket(const Id market_id) {
    const auto rows = database_.query(std::format(
        "SELECT id,book_id,target_key,target_kind,dimension_name,block_x,block_y,block_z,actor_id,item_type,item_data,"
        "item_nbt,item_name,active,created_by FROM exchange_markets WHERE id={}",
        market_id));
    return rows.empty() ? std::nullopt : std::optional<Market>(marketFromRow(rows.front()));
}

std::vector<Market> ExchangeService::activeMarkets() {
    const auto rows = database_.query(
        "SELECT m.id,m.book_id,m.target_key,m.target_kind,m.dimension_name,m.block_x,m.block_y,m.block_z,m.actor_id,"
        "m.item_type,m.item_data,m.item_nbt,m.item_name,m.active,m.created_by FROM exchange_target_bindings b "
        "JOIN exchange_markets m ON m.id=b.market_id WHERE m.active=1 ORDER BY m.id");
    std::vector<Market> result;
    result.reserve(rows.size());
    for (const auto &row : rows) {
        result.push_back(marketFromRow(row));
    }
    return result;
}

OrderBook ExchangeService::orderBook(const Id market_id, const int depth) {
    if (depth <= 0 || depth > 50) {
        throw std::runtime_error("order-book depth must be between 1 and 50");
    }
    const auto market =
        database_.query(std::format("SELECT book_id FROM exchange_markets WHERE id={}", market_id));
    if (market.empty()) {
        throw std::runtime_error("market does not exist");
    }
    const auto book_id = static_cast<Id>(cellInt64(market.front(), 0));
    OrderBook result;
    const auto bids = database_.query(std::format(
        "SELECT o.price_cents,SUM(o.remaining_qty) FROM exchange_orders o "
        "JOIN exchange_markets m ON m.id=o.market_id WHERE m.book_id={} AND o.side='BUY' "
        "AND o.order_type='LIMIT' AND o.status IN ('OPEN','PARTIAL') GROUP BY o.price_cents "
                    "ORDER BY price_cents DESC LIMIT {}",
        book_id, depth));
    for (const auto &row : bids) {
        result.bids.push_back({cellInt64(row, 0), cellInt(row, 1)});
    }
    const auto asks = database_.query(std::format(
        "SELECT o.price_cents,SUM(o.remaining_qty) FROM exchange_orders o "
        "JOIN exchange_markets m ON m.id=o.market_id WHERE m.book_id={} AND o.side='SELL' "
        "AND o.order_type='LIMIT' AND o.status IN ('OPEN','PARTIAL') GROUP BY o.price_cents "
                    "ORDER BY price_cents ASC LIMIT {}",
        book_id, depth));
    for (const auto &row : asks) {
        result.asks.push_back({cellInt64(row, 0), cellInt(row, 1)});
    }
    const auto last = database_.query(std::format(
        "SELECT t.price_cents FROM exchange_trades t JOIN exchange_markets m ON m.id=t.market_id "
        "WHERE m.book_id={} ORDER BY t.id DESC LIMIT 1",
        book_id));
    if (!last.empty()) {
        result.last_price_cents = cellInt64(last.front(), 0);
    }
    return result;
}

std::unordered_map<Id, OrderBook> ExchangeService::topOfBooks(const std::vector<Id> &market_ids) {
    std::unordered_map<Id, OrderBook> result;
    if (market_ids.empty()) {
        return result;
    }
    for (const auto id : market_ids) {
        result.try_emplace(id);
    }
    const auto ids = idList(market_ids);
    const auto market_books = database_.query(
        std::format("SELECT id,book_id FROM exchange_markets WHERE id IN ({})", ids));
    std::unordered_map<Id, Id> book_by_market;
    std::vector<Id> book_ids;
    for (const auto &row : market_books) {
        const auto market_id = static_cast<Id>(cellInt64(row, 0));
        const auto book_id = static_cast<Id>(cellInt64(row, 1));
        book_by_market[market_id] = book_id;
        if (std::find(book_ids.begin(), book_ids.end(), book_id) == book_ids.end()) {
            book_ids.push_back(book_id);
        }
    }
    if (book_ids.empty()) {
        return result;
    }
    const auto books = idList(book_ids);
    std::unordered_map<Id, OrderBook> by_book;
    const auto bids = database_.query(std::format(
        "SELECT m.book_id,o.price_cents,LEAST(SUM(o.remaining_qty),2147483647) FROM exchange_orders o "
        "JOIN exchange_markets m ON m.id=o.market_id "
        "JOIN (SELECT m2.book_id,MAX(o2.price_cents) price_cents FROM exchange_orders o2 "
        "JOIN exchange_markets m2 ON m2.id=o2.market_id WHERE m2.book_id IN ({}) AND o2.side='BUY' "
        "AND o2.order_type='LIMIT' AND o2.status IN ('OPEN','PARTIAL') GROUP BY m2.book_id) best "
        "ON best.book_id=m.book_id AND best.price_cents=o.price_cents WHERE o.side='BUY' "
        "AND o.order_type='LIMIT' AND o.status IN ('OPEN','PARTIAL') GROUP BY m.book_id,o.price_cents",
        books));
    for (const auto &row : bids) {
        by_book[static_cast<Id>(cellInt64(row, 0))].bids.push_back({cellInt64(row, 1), cellInt(row, 2)});
    }
    const auto asks = database_.query(std::format(
        "SELECT m.book_id,o.price_cents,LEAST(SUM(o.remaining_qty),2147483647) FROM exchange_orders o "
        "JOIN exchange_markets m ON m.id=o.market_id "
        "JOIN (SELECT m2.book_id,MIN(o2.price_cents) price_cents FROM exchange_orders o2 "
        "JOIN exchange_markets m2 ON m2.id=o2.market_id WHERE m2.book_id IN ({}) AND o2.side='SELL' "
        "AND o2.order_type='LIMIT' AND o2.status IN ('OPEN','PARTIAL') GROUP BY m2.book_id) best "
        "ON best.book_id=m.book_id AND best.price_cents=o.price_cents WHERE o.side='SELL' "
        "AND o.order_type='LIMIT' AND o.status IN ('OPEN','PARTIAL') GROUP BY m.book_id,o.price_cents",
        books));
    for (const auto &row : asks) {
        by_book[static_cast<Id>(cellInt64(row, 0))].asks.push_back({cellInt64(row, 1), cellInt(row, 2)});
    }
    const auto trades = database_.query(std::format(
        "SELECT m.book_id,t.price_cents FROM exchange_trades t JOIN exchange_markets m ON m.id=t.market_id "
        "JOIN (SELECT m2.book_id,MAX(t2.id) trade_id FROM exchange_trades t2 "
        "JOIN exchange_markets m2 ON m2.id=t2.market_id WHERE m2.book_id IN ({}) GROUP BY m2.book_id) latest "
        "ON latest.trade_id=t.id",
        books));
    for (const auto &row : trades) {
        by_book[static_cast<Id>(cellInt64(row, 0))].last_price_cents = cellInt64(row, 1);
    }
    for (const auto &[market_id, book_id] : book_by_market) {
        if (const auto book = by_book.find(book_id); book != by_book.end()) {
            result[market_id] = book->second;
        }
    }
    return result;
}

ExecutionResult ExchangeService::placeOrder(const OrderRequest &request) {
    validateRequest(request);
    ensureAccount(request.player_uuid, request.player_name);
    return request.side == Side::Buy ? placeBuy(request) : placeSell(request);
}

ExecutionResult ExchangeService::placeBuy(const OrderRequest &request) {
    Transaction transaction(database_);
    const auto book_id = lockActiveBook(request.market_id);

    auto available_balance = lockedBalance(request.player_uuid);
    Cents reserved = 0;
    if (request.type == OrderType::Limit) {
        reserved = checkedProduct(request.price_cents, request.quantity);
        if (available_balance < reserved) {
            throw std::runtime_error("insufficient exchange balance");
        }
    }

    database_.execute(std::format(
        "INSERT INTO exchange_orders(market_id,player_uuid,side,order_type,price_cents,original_qty,remaining_qty,"
        "reserved_cents,status) VALUES ({},{},'BUY',{}, {},{},{},{},'OPEN')",
        request.market_id, database_.quote(request.player_uuid), database_.quote(toSql(request.type)),
        request.type == OrderType::Limit ? request.price_cents : 0, request.quantity, request.quantity, reserved));
    const auto order_id = static_cast<Id>(database_.lastInsertId());
    if (reserved > 0) {
        debit(request.player_uuid, reserved, "BUY_ORDER_RESERVE", "ORDER", order_id);
        available_balance -= reserved;
    }

    const auto price_filter =
        request.type == OrderType::Limit ? std::format("AND o.price_cents<={}", request.price_cents) : std::string{};
    const auto asks = database_.query(std::format(
        "SELECT o.id,o.player_uuid,o.price_cents,o.remaining_qty FROM exchange_orders o "
        "JOIN exchange_markets m ON m.id=o.market_id WHERE m.book_id={} AND o.side='SELL' "
        "AND o.order_type='LIMIT' AND o.status IN ('OPEN','PARTIAL') AND o.player_uuid<>{} {} "
        "ORDER BY o.price_cents ASC,o.id ASC FOR UPDATE",
        book_id, database_.quote(request.player_uuid), price_filter));

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
            debit(request.player_uuid, cost, "MARKET_BUY_TRADE", "ORDER", order_id);
            available_balance -= cost;
        } else {
            const auto refund = checkedProduct(request.price_cents - price, fill);
            if (refund > 0) {
                credit(request.player_uuid, refund, "PRICE_IMPROVEMENT_REFUND", "ORDER", order_id);
                available_balance += refund;
            }
        }

        const auto ask_after = ask_remaining - fill;
        database_.execute(std::format("UPDATE exchange_orders SET remaining_qty={},status='{}' WHERE id={}", ask_after,
                                      ask_after == 0 ? "FILLED" : "PARTIAL", ask_id));
        const auto cost = checkedProduct(price, fill);
        credit(seller, cost, "TRADE_PROCEEDS", "ORDER", ask_id);
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

    const auto current_reserved = request.type == OrderType::Limit ? checkedProduct(request.price_cents, remaining) : 0;
    const auto status =
        request.type == OrderType::Market
            ? (filled_total == request.quantity ? "FILLED" : (filled_total > 0 ? "PARTIAL" : "CANCELED"))
            : orderStatus(remaining, filled_total);
    const auto persisted_remaining = request.type == OrderType::Limit ? remaining : 0;
    database_.execute(
        std::format("UPDATE exchange_orders SET remaining_qty={},reserved_cents={},status='{}' WHERE id={}",
                    persisted_remaining, current_reserved, status, order_id));
    const auto final_balance = lockedBalance(request.player_uuid);
    transaction.commit();
    return {order_id, request.quantity, filled_total, request.type == OrderType::Limit ? remaining : 0,
            gross,    final_balance};
}

ExecutionResult ExchangeService::placeSell(const OrderRequest &request) {
    Transaction transaction(database_);
    auto result = placeSellLocked(request);
    transaction.commit();
    return result;
}

ExecutionResult ExchangeService::placeSellLocked(const OrderRequest &request) {
    const auto book_id = lockActiveBook(request.market_id);
    static_cast<void>(lockedBalance(request.player_uuid));

    database_.execute(std::format(
        "INSERT INTO exchange_orders(market_id,player_uuid,side,order_type,price_cents,original_qty,remaining_qty,"
        "reserved_cents,status) VALUES ({},{},'SELL',{}, {},{},{},0,'OPEN')",
        request.market_id, database_.quote(request.player_uuid), database_.quote(toSql(request.type)),
        request.type == OrderType::Limit ? request.price_cents : 0, request.quantity, request.quantity));
    const auto order_id = static_cast<Id>(database_.lastInsertId());

    const auto price_filter =
        request.type == OrderType::Limit ? std::format("AND o.price_cents>={}", request.price_cents) : std::string{};
    const auto bids = database_.query(std::format(
        "SELECT o.id,o.player_uuid,o.price_cents,o.remaining_qty,o.reserved_cents FROM exchange_orders o "
        "JOIN exchange_markets m ON m.id=o.market_id WHERE m.book_id={} AND o.side='BUY' "
        "AND o.order_type='LIMIT' AND o.status IN ('OPEN','PARTIAL') AND o.player_uuid<>{} {} "
        "ORDER BY o.price_cents DESC,o.id ASC FOR UPDATE",
        book_id, database_.quote(request.player_uuid), price_filter));

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
        database_.execute(
            std::format("UPDATE exchange_orders SET remaining_qty={},reserved_cents={},status='{}' WHERE id={}",
                        bid_after, reserved_after, bid_after == 0 ? "FILLED" : "PARTIAL", bid_id));
        const auto proceeds = checkedProduct(price, fill);
        credit(request.player_uuid, proceeds, "TRADE_PROCEEDS", "ORDER", order_id);
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
    const auto status =
        request.type == OrderType::Market
            ? (filled_total == request.quantity ? "FILLED" : (filled_total > 0 ? "PARTIAL" : "CANCELED"))
            : orderStatus(remaining, filled_total);
    const auto persisted_remaining = request.type == OrderType::Limit ? remaining : 0;
    database_.execute(std::format("UPDATE exchange_orders SET remaining_qty={},status='{}' WHERE id={}",
                                  persisted_remaining, status, order_id));
    const auto final_balance = lockedBalance(request.player_uuid);
    return {order_id, request.quantity, filled_total, request.type == OrderType::Limit ? remaining : 0,
            gross,    final_balance};
}

void ExchangeService::cancelOrder(const Id order_id, const std::string_view player_uuid, const bool administrator) {
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
        credit(owner, reserved, "ORDER_CANCEL_REFUND", "ORDER", order_id);
    } else if (remaining > 0) {
        addDelivery(owner, market_id, remaining, "ORDER_CANCELED");
    }
    database_.execute(std::format(
        "UPDATE exchange_orders SET remaining_qty=0,reserved_cents=0,status='CANCELED' WHERE id={}", order_id));
    transaction.commit();
}

std::vector<OpenOrder> ExchangeService::openOrders(const std::string_view player_uuid) {
    const auto rows = database_.query(
        std::format("SELECT o.id,o.market_id,m.item_type,m.item_data,m.item_nbt,m.item_name,o.side,o.price_cents,"
                    "o.remaining_qty FROM exchange_orders o "
                    "JOIN exchange_markets m ON m.id=o.market_id WHERE o.player_uuid={} AND o.order_type='LIMIT' "
                    "AND o.status IN ('OPEN','PARTIAL') ORDER BY o.id DESC",
                    database_.quote(player_uuid)));
    std::vector<OpenOrder> result;
    result.reserve(rows.size());
    for (const auto &row : rows) {
        result.push_back({static_cast<Id>(cellInt64(row, 0)),
                          static_cast<Id>(cellInt64(row, 1)),
                          {cellString(row, 2), cellInt(row, 3), bytesFromCell(row, 4), cellString(row, 5)},
                          cellString(row, 6) == "BUY" ? Side::Buy : Side::Sell,
                          cellInt64(row, 7),
                          cellInt(row, 8)});
    }
    return result;
}

std::vector<Delivery> ExchangeService::pendingDeliveries(const std::string_view player_uuid) {
    const auto rows = database_.query(
        std::format("SELECT d.id,d.market_id,m.item_type,m.item_data,m.item_nbt,m.item_name,d.quantity,d.claimed_qty,"
                    "d.reserved_qty,d.reason "
                    "FROM exchange_deliveries d JOIN exchange_markets m ON m.id=d.market_id WHERE d.player_uuid={} "
                    "AND d.claimed_qty+d.reserved_qty<d.quantity ORDER BY d.id",
                    database_.quote(player_uuid)));
    std::vector<Delivery> result;
    result.reserve(rows.size());
    for (const auto &row : rows) {
        result.push_back({static_cast<Id>(cellInt64(row, 0)),
                          static_cast<Id>(cellInt64(row, 1)),
                          {cellString(row, 2), cellInt(row, 3), bytesFromCell(row, 4), cellString(row, 5)},
                          cellInt(row, 6),
                          cellInt(row, 7),
                          cellInt(row, 8),
                          cellString(row, 9)});
    }
    return result;
}

std::vector<DeliveryClaim> ExchangeService::unfinishedDeliveryClaims(const std::string_view player_uuid) {
    const auto rows = database_.query(std::format(
        "SELECT c.id,c.delivery_id,m.item_type,m.item_data,m.item_nbt,m.item_name,c.quantity,c.applied_qty,c.status,"
        "c.cleaned_at IS NOT NULL FROM exchange_delivery_claims c "
        "JOIN exchange_deliveries d ON d.id=c.delivery_id JOIN exchange_markets m ON m.id=d.market_id "
        "WHERE c.player_uuid={} AND (c.status='PREPARED' OR (c.status='APPLIED' AND c.cleaned_at IS NULL)) "
        "ORDER BY c.id",
        database_.quote(player_uuid)));
    std::vector<DeliveryClaim> result;
    result.reserve(rows.size());
    for (const auto &row : rows) {
        result.push_back(deliveryClaimFromRow(row));
    }
    return result;
}

std::optional<DeliveryClaim> ExchangeService::findDeliveryClaim(const Id claim_id,
                                                                const std::string_view player_uuid) {
    const auto rows = database_.query(std::format(
        "SELECT c.id,c.delivery_id,m.item_type,m.item_data,m.item_nbt,m.item_name,c.quantity,c.applied_qty,c.status,"
        "c.cleaned_at IS NOT NULL FROM exchange_delivery_claims c "
        "JOIN exchange_deliveries d ON d.id=c.delivery_id JOIN exchange_markets m ON m.id=d.market_id "
        "WHERE c.id={} AND c.player_uuid={}",
        claim_id, database_.quote(player_uuid)));
    return rows.empty() ? std::nullopt : std::optional<DeliveryClaim>(deliveryClaimFromRow(rows.front()));
}

DeliveryClaim ExchangeService::prepareDeliveryClaim(const Id delivery_id, const std::string_view player_uuid,
                                                    const int quantity) {
    if (quantity <= 0 || player_uuid.empty()) {
        throw std::runtime_error("delivery claim data is incomplete");
    }
    Transaction transaction(database_);
    const auto rows = database_.query(std::format(
        "SELECT d.player_uuid,d.quantity,d.claimed_qty,d.reserved_qty,m.item_type,m.item_data,m.item_nbt,m.item_name "
        "FROM exchange_deliveries d JOIN exchange_markets m ON m.id=d.market_id WHERE d.id={} FOR UPDATE",
        delivery_id));
    if (rows.empty()) {
        throw std::runtime_error("delivery does not exist");
    }
    if (cellString(rows.front(), 0) != player_uuid) {
        throw std::runtime_error("delivery belongs to another player");
    }
    const auto total = cellInt(rows.front(), 1);
    const auto claimed = cellInt(rows.front(), 2);
    const auto reserved = cellInt(rows.front(), 3);
    const auto available = total - claimed - reserved;
    if (available <= 0) {
        throw std::runtime_error("delivery has no unreserved quantity");
    }
    const auto prepared = std::min(quantity, available);
    database_.execute(
        std::format("INSERT INTO exchange_delivery_claims(delivery_id,player_uuid,quantity) VALUES ({},{},{})",
                    delivery_id, database_.quote(player_uuid), prepared));
    const auto claim_id = static_cast<Id>(database_.lastInsertId());
    database_.execute(
        std::format("UPDATE exchange_deliveries SET reserved_qty=reserved_qty+{} WHERE id={}", prepared, delivery_id));
    transaction.commit();
    return {claim_id,
            delivery_id,
            {cellString(rows.front(), 4), cellInt(rows.front(), 5), bytesFromCell(rows.front(), 6),
             cellString(rows.front(), 7)},
            prepared,
            0,
            DeliveryClaimStatus::Prepared,
            false};
}

void ExchangeService::completeDeliveryClaim(const Id claim_id, const int delivered_quantity) {
    if (delivered_quantity < 0) {
        throw std::runtime_error("delivered quantity must not be negative");
    }
    Transaction transaction(database_);
    const auto claim = database_.query(std::format(
        "SELECT delivery_id,quantity,applied_qty,status FROM exchange_delivery_claims WHERE id={} FOR UPDATE",
        claim_id));
    if (claim.empty()) {
        throw std::runtime_error("delivery claim does not exist");
    }
    const auto delivery_id = static_cast<Id>(cellInt64(claim.front(), 0));
    const auto prepared = cellInt(claim.front(), 1);
    const auto applied = cellInt(claim.front(), 2);
    const auto status = cellString(claim.front(), 3);
    if (status != "PREPARED") {
        if ((status == "APPLIED" && applied == delivered_quantity) ||
            (status == "CANCELED" && delivered_quantity == 0)) {
            transaction.commit();
            return;
        }
        throw std::runtime_error("delivery claim was already completed differently");
    }
    if (delivered_quantity > prepared) {
        throw std::runtime_error("delivered quantity exceeds prepared claim");
    }
    const auto delivery = database_.query(std::format(
        "SELECT quantity,claimed_qty,reserved_qty FROM exchange_deliveries WHERE id={} FOR UPDATE", delivery_id));
    if (delivery.empty()) {
        throw std::runtime_error("delivery for claim does not exist");
    }
    const auto total = cellInt(delivery.front(), 0);
    const auto claimed = cellInt(delivery.front(), 1);
    const auto reserved = cellInt(delivery.front(), 2);
    if (reserved < prepared || claimed + delivered_quantity > total) {
        throw std::runtime_error("delivery reservation invariant failed");
    }
    database_.execute(std::format(
        "UPDATE exchange_deliveries SET claimed_qty=claimed_qty+{},reserved_qty=reserved_qty-{} WHERE id={}",
        delivered_quantity, prepared, delivery_id));
    if (delivered_quantity == 0) {
        database_.execute(std::format(
            "UPDATE exchange_delivery_claims SET applied_qty=0,status='CANCELED',applied_at=CURRENT_TIMESTAMP(6),"
            "cleaned_at=CURRENT_TIMESTAMP(6) WHERE id={}",
            claim_id));
    } else {
        database_.execute(std::format(
            "UPDATE exchange_delivery_claims SET applied_qty={},status='APPLIED',applied_at=CURRENT_TIMESTAMP(6) "
            "WHERE id={}",
            delivered_quantity, claim_id));
    }
    transaction.commit();
}

void ExchangeService::markDeliveryClaimCleaned(const Id claim_id) {
    database_.execute(std::format(
        "UPDATE exchange_delivery_claims SET cleaned_at=CURRENT_TIMESTAMP(6) WHERE id={} AND status='APPLIED'",
        claim_id));
    if (database_.affectedRows() != 1) {
        throw std::runtime_error("applied delivery claim does not exist");
    }
}

SellEscrow ExchangeService::prepareSellEscrow(const OrderRequest &request) {
    validateRequest(request);
    if (request.side != Side::Sell) {
        throw std::runtime_error("sell escrow requires a sell order");
    }
    ensureAccount(request.player_uuid, request.player_name);
    Transaction transaction(database_);
    const auto market = database_.query(
        std::format("SELECT active,item_type,item_data,item_nbt,item_name FROM exchange_markets WHERE id={} FOR UPDATE",
                    request.market_id));
    if (market.empty() || cellInt(market.front(), 0) == 0) {
        throw std::runtime_error("market is not active");
    }
    database_.execute(std::format(
        "INSERT INTO exchange_sell_escrows(market_id,player_uuid,player_name,order_type,price_cents,requested_qty) "
        "VALUES ({},{},{},{},{},{})",
        request.market_id, database_.quote(request.player_uuid), database_.quote(request.player_name),
        database_.quote(toSql(request.type)), request.type == OrderType::Limit ? request.price_cents : 0,
        request.quantity));
    const auto escrow_id = static_cast<Id>(database_.lastInsertId());
    transaction.commit();
    return {escrow_id,
            request.market_id,
            {cellString(market.front(), 1), cellInt(market.front(), 2), bytesFromCell(market.front(), 3),
             cellString(market.front(), 4)},
            request.player_uuid,
            request.player_name,
            request.type,
            request.type == OrderType::Limit ? request.price_cents : 0,
            request.quantity,
            0,
            0,
            SellEscrowStatus::Prepared,
            std::nullopt,
            false};
}

void ExchangeService::markSellEscrowTagged(const Id escrow_id, const int tagged_quantity, const int receipt_count) {
    if (tagged_quantity <= 0 || receipt_count <= 0) {
        throw std::runtime_error("tagged sell escrow quantities must be positive");
    }
    Transaction transaction(database_);
    const auto rows = database_.query(std::format(
        "SELECT requested_qty,tagged_qty,receipt_count,status FROM exchange_sell_escrows WHERE id={} FOR UPDATE",
        escrow_id));
    if (rows.empty()) {
        throw std::runtime_error("sell escrow does not exist");
    }
    const auto requested = cellInt(rows.front(), 0);
    const auto existing_tagged = cellInt(rows.front(), 1);
    const auto existing_receipts = cellInt(rows.front(), 2);
    const auto status = cellString(rows.front(), 3);
    if (tagged_quantity < requested) {
        throw std::runtime_error("tagged sell quantity is smaller than the order");
    }
    if (status == "TAGGED" && existing_tagged == tagged_quantity && existing_receipts == receipt_count) {
        transaction.commit();
        return;
    }
    if (status != "PREPARED") {
        throw std::runtime_error("sell escrow is not waiting for item tags");
    }
    database_.execute(std::format("UPDATE exchange_sell_escrows SET tagged_qty={},receipt_count={},status='TAGGED',"
                                  "tagged_at=CURRENT_TIMESTAMP(6) WHERE id={}",
                                  tagged_quantity, receipt_count, escrow_id));
    transaction.commit();
}

ExecutionResult ExchangeService::executeSellEscrow(const Id escrow_id) {
    Transaction transaction(database_);
    const auto rows = database_.query(std::format(
        "SELECT market_id,player_uuid,player_name,order_type,price_cents,requested_qty,tagged_qty,status,order_id "
        "FROM exchange_sell_escrows WHERE id={} FOR UPDATE",
        escrow_id));
    if (rows.empty()) {
        throw std::runtime_error("sell escrow does not exist");
    }
    const auto status = cellString(rows.front(), 7);
    if (status == "ORDERED") {
        const auto existing_order_id = static_cast<Id>(*optionalInt64(rows.front(), 8));
        const auto order =
            database_.query(std::format("SELECT o.original_qty,o.remaining_qty,COALESCE(SUM(t.quantity),0),"
                                        "COALESCE(SUM(t.price_cents*t.quantity),0) FROM exchange_orders o "
                                        "LEFT JOIN exchange_trades t ON t.sell_order_id=o.id WHERE o.id={} "
                                        "GROUP BY o.id,o.original_qty,o.remaining_qty",
                                        existing_order_id));
        if (order.empty()) {
            throw std::runtime_error("ordered sell escrow has no order");
        }
        const auto requested = cellInt(order.front(), 0);
        const auto remaining = cellInt(order.front(), 1);
        const auto filled = cellInt(order.front(), 2);
        const auto owner = cellString(rows.front(), 1);
        const auto final_balance = lockedBalance(owner);
        transaction.commit();
        return {existing_order_id, requested, filled, remaining, cellInt64(order.front(), 3), final_balance};
    }
    if (status != "TAGGED") {
        throw std::runtime_error("sell escrow is not ready to become an order");
    }

    OrderRequest request;
    request.market_id = static_cast<Id>(cellInt64(rows.front(), 0));
    request.player_uuid = cellString(rows.front(), 1);
    request.player_name = cellString(rows.front(), 2);
    request.side = Side::Sell;
    request.type = cellString(rows.front(), 3) == "LIMIT" ? OrderType::Limit : OrderType::Market;
    request.price_cents = cellInt64(rows.front(), 4);
    request.quantity = cellInt(rows.front(), 5);
    const auto tagged_quantity = cellInt(rows.front(), 6);
    auto result = placeSellLocked(request);
    if (tagged_quantity > request.quantity) {
        addDelivery(request.player_uuid, request.market_id, tagged_quantity - request.quantity, "ESCROW_EXCESS");
    }
    database_.execute(std::format(
        "UPDATE exchange_sell_escrows SET status='ORDERED',order_id={},ordered_at=CURRENT_TIMESTAMP(6) WHERE id={}",
        result.order_id, escrow_id));
    transaction.commit();
    return result;
}

void ExchangeService::cancelSellEscrow(const Id escrow_id) {
    Transaction transaction(database_);
    const auto rows =
        database_.query(std::format("SELECT status FROM exchange_sell_escrows WHERE id={} FOR UPDATE", escrow_id));
    if (rows.empty()) {
        throw std::runtime_error("sell escrow does not exist");
    }
    const auto status = cellString(rows.front(), 0);
    if (status == "CANCELED") {
        transaction.commit();
        return;
    }
    if (status != "PREPARED") {
        throw std::runtime_error("only a prepared sell escrow can be canceled");
    }
    database_.execute(std::format(
        "UPDATE exchange_sell_escrows SET status='CANCELED',cleaned_at=CURRENT_TIMESTAMP(6) WHERE id={}", escrow_id));
    transaction.commit();
}

void ExchangeService::markSellEscrowCleaned(const Id escrow_id) {
    database_.execute(
        std::format("UPDATE exchange_sell_escrows SET cleaned_at=CURRENT_TIMESTAMP(6) WHERE id={} AND status='ORDERED'",
                    escrow_id));
    if (database_.affectedRows() != 1) {
        throw std::runtime_error("ordered sell escrow does not exist");
    }
}

std::vector<SellEscrow> ExchangeService::unfinishedSellEscrows(const std::string_view player_uuid) {
    const auto rows = database_.query(std::format(
        "SELECT e.id,e.market_id,m.item_type,m.item_data,m.item_nbt,m.item_name,e.player_uuid,e.player_name,"
        "e.order_type,e.price_cents,e.requested_qty,e.tagged_qty,e.receipt_count,e.status,e.order_id,"
        "e.cleaned_at IS NOT NULL FROM exchange_sell_escrows e JOIN exchange_markets m ON m.id=e.market_id "
        "WHERE e.player_uuid={} AND e.status<>'CANCELED' AND e.cleaned_at IS NULL ORDER BY e.id",
        database_.quote(player_uuid)));
    std::vector<SellEscrow> result;
    result.reserve(rows.size());
    for (const auto &row : rows) {
        result.push_back(sellEscrowFromRow(row));
    }
    return result;
}

std::optional<SellEscrow> ExchangeService::findSellEscrow(const Id escrow_id,
                                                           const std::string_view player_uuid) {
    const auto rows = database_.query(std::format(
        "SELECT e.id,e.market_id,m.item_type,m.item_data,m.item_nbt,m.item_name,e.player_uuid,e.player_name,"
        "e.order_type,e.price_cents,e.requested_qty,e.tagged_qty,e.receipt_count,e.status,e.order_id,"
        "e.cleaned_at IS NOT NULL FROM exchange_sell_escrows e JOIN exchange_markets m ON m.id=e.market_id "
        "WHERE e.id={} AND e.player_uuid={}",
        escrow_id, database_.quote(player_uuid)));
    return rows.empty() ? std::nullopt : std::optional<SellEscrow>(sellEscrowFromRow(rows.front()));
}

EconomyTransfer ExchangeService::prepareEconomyTransfer(const std::string_view player_uuid,
                                                        const std::string_view player_name,
                                                        const EconomyTransferDirection direction,
                                                        const Cents amount_cents,
                                                        const std::int64_t amount_units) {
    if (player_uuid.empty() || player_name.empty()) {
        throw std::runtime_error("economy transfer identity is incomplete");
    }
    if (amount_cents <= 0 || amount_units <= 0) {
        throw std::runtime_error("economy transfer amount must be positive");
    }
    ensureAccount(player_uuid, player_name);

    Transaction transaction(database_);
    database_.execute(std::format(
        "INSERT INTO exchange_economy_transfers("
        "operation_key,player_uuid,player_name,direction,amount_cents,amount_units) "
        "VALUES (UUID(),{},{},{},{},{})",
        database_.quote(player_uuid), database_.quote(player_name), database_.quote(toSql(direction)), amount_cents,
        amount_units));
    const auto transfer_id = static_cast<Id>(database_.lastInsertId());
    if (direction == EconomyTransferDirection::Withdraw) {
        debit(player_uuid, amount_cents, "UMONEY_WITHDRAW_RESERVED", "ECONOMY_TRANSFER", transfer_id);
    }
    transaction.commit();

    const auto transfer = findEconomyTransfer(transfer_id);
    if (!transfer) {
        throw std::runtime_error("prepared economy transfer could not be reloaded");
    }
    return *transfer;
}

std::vector<EconomyTransfer> ExchangeService::pendingEconomyTransfers(const int limit) {
    if (limit <= 0 || limit > 100) {
        throw std::runtime_error("economy transfer query limit must be between 1 and 100");
    }
    const auto rows = database_.query(std::format(
        "SELECT id,operation_key,player_uuid,player_name,direction,amount_cents,amount_units,status,external_balance_units,"
        "attempt_count,COALESCE(last_error,'') FROM exchange_economy_transfers "
        "WHERE status IN ('PREPARED','EXTERNAL_APPLIED') ORDER BY id LIMIT {}",
        limit));
    std::vector<EconomyTransfer> result;
    result.reserve(rows.size());
    for (const auto &row : rows) {
        result.push_back(economyTransferFromRow(row));
    }
    return result;
}

std::optional<EconomyTransfer> ExchangeService::findEconomyTransfer(const Id transfer_id) {
    const auto rows = database_.query(std::format(
        "SELECT id,operation_key,player_uuid,player_name,direction,amount_cents,amount_units,status,external_balance_units,"
        "attempt_count,COALESCE(last_error,'') FROM exchange_economy_transfers WHERE id={}",
        transfer_id));
    return rows.empty() ? std::nullopt : std::optional<EconomyTransfer>(economyTransferFromRow(rows.front()));
}

void ExchangeService::recordEconomyTransferAttempt(const Id transfer_id, const std::string_view error) {
    const auto safe_error = std::string(error.substr(0, 500));
    database_.execute(std::format(
        "UPDATE exchange_economy_transfers SET attempt_count=attempt_count+1,last_error={} "
        "WHERE id={} AND status='PREPARED'",
        database_.quote(safe_error), transfer_id));
    if (database_.affectedRows() != 1) {
        const auto transfer = findEconomyTransfer(transfer_id);
        if (!transfer || transfer->status == EconomyTransferStatus::Failed) {
            throw std::runtime_error("prepared economy transfer does not exist");
        }
    }
}

void ExchangeService::markEconomyTransferExternalApplied(const Id transfer_id,
                                                         const std::int64_t external_balance_units) {
    Transaction transaction(database_);
    const auto rows = database_.query(
        std::format("SELECT status FROM exchange_economy_transfers WHERE id={} FOR UPDATE", transfer_id));
    if (rows.empty()) {
        throw std::runtime_error("economy transfer does not exist");
    }
    const auto status = economyTransferStatus(cellString(rows.front(), 0));
    if (status == EconomyTransferStatus::Failed || status == EconomyTransferStatus::Blocked) {
        throw std::runtime_error("failed or blocked economy transfer cannot be applied");
    }
    if (status == EconomyTransferStatus::Prepared) {
        database_.execute(std::format(
            "UPDATE exchange_economy_transfers SET status='EXTERNAL_APPLIED',external_balance_units={},"
            "external_applied_at=CURRENT_TIMESTAMP(6),attempt_count=attempt_count+1,last_error=NULL WHERE id={}",
            external_balance_units, transfer_id));
    }
    transaction.commit();
}

Cents ExchangeService::completeEconomyTransfer(const Id transfer_id) {
    Transaction transaction(database_);
    const auto rows = database_.query(std::format(
        "SELECT player_uuid,direction,amount_cents,status FROM exchange_economy_transfers WHERE id={} FOR UPDATE",
        transfer_id));
    if (rows.empty()) {
        throw std::runtime_error("economy transfer does not exist");
    }
    const auto player_uuid = cellString(rows.front(), 0);
    const auto direction = cellString(rows.front(), 1);
    const auto amount_cents = cellInt64(rows.front(), 2);
    const auto status = economyTransferStatus(cellString(rows.front(), 3));
    if (status == EconomyTransferStatus::Failed) {
        throw std::runtime_error("failed economy transfer cannot be completed");
    }
    if (status == EconomyTransferStatus::Completed) {
        const auto result = lockedBalance(player_uuid);
        transaction.commit();
        return result;
    }
    if (status != EconomyTransferStatus::ExternalApplied) {
        throw std::runtime_error("economy transfer external operation is not applied");
    }
    if (direction == "DEPOSIT") {
        credit(player_uuid, amount_cents, "UMONEY_DEPOSIT", "ECONOMY_TRANSFER", transfer_id);
    }
    database_.execute(std::format(
        "UPDATE exchange_economy_transfers SET status='COMPLETED',completed_at=CURRENT_TIMESTAMP(6),last_error=NULL "
        "WHERE id={}",
        transfer_id));
    const auto result = lockedBalance(player_uuid);
    transaction.commit();
    return result;
}

Cents ExchangeService::failEconomyTransfer(const Id transfer_id, const std::string_view error) {
    Transaction transaction(database_);
    const auto rows = database_.query(std::format(
        "SELECT player_uuid,direction,amount_cents,status FROM exchange_economy_transfers WHERE id={} FOR UPDATE",
        transfer_id));
    if (rows.empty()) {
        throw std::runtime_error("economy transfer does not exist");
    }
    const auto player_uuid = cellString(rows.front(), 0);
    const auto direction = cellString(rows.front(), 1);
    const auto amount_cents = cellInt64(rows.front(), 2);
    const auto status = economyTransferStatus(cellString(rows.front(), 3));
    if (status == EconomyTransferStatus::ExternalApplied || status == EconomyTransferStatus::Completed ||
        status == EconomyTransferStatus::Blocked) {
        throw std::runtime_error("applied economy transfer cannot be failed");
    }
    if (status == EconomyTransferStatus::Prepared) {
        if (direction == "WITHDRAW") {
            credit(player_uuid, amount_cents, "UMONEY_WITHDRAW_REFUND", "ECONOMY_TRANSFER", transfer_id);
        }
        const auto safe_error = std::string(error.substr(0, 500));
        database_.execute(std::format(
            "UPDATE exchange_economy_transfers SET status='FAILED',attempt_count=attempt_count+1,last_error={},"
            "completed_at=CURRENT_TIMESTAMP(6) WHERE id={}",
            database_.quote(safe_error), transfer_id));
    }
    const auto result = lockedBalance(player_uuid);
    transaction.commit();
    return result;
}

void ExchangeService::blockEconomyTransfer(const Id transfer_id, const std::string_view error) {
    const auto safe_error = std::string(error.substr(0, 500));
    database_.execute(std::format(
        "UPDATE exchange_economy_transfers SET status='BLOCKED',attempt_count=attempt_count+1,last_error={} "
        "WHERE id={} AND status='PREPARED'",
        database_.quote(safe_error), transfer_id));
    if (database_.affectedRows() != 1) {
        throw std::runtime_error("prepared economy transfer does not exist");
    }
}

int ExchangeService::pendingEconomyTransferCount() {
    const auto rows = database_.query(
        "SELECT COUNT(*) FROM exchange_economy_transfers "
        "WHERE status IN ('PREPARED','EXTERNAL_APPLIED','BLOCKED')");
    return rows.empty() ? 0 : cellInt(rows.front(), 0);
}

Cents ExchangeService::economyBackingDeficit() {
    const auto rows = database_.query(
        "SELECT "
        "(SELECT COALESCE(SUM(balance_cents),0) FROM exchange_accounts)+"
        "(SELECT COALESCE(SUM(reserved_cents),0) FROM exchange_orders "
        " WHERE status IN ('OPEN','PARTIAL'))+"
        "(SELECT COALESCE(SUM(amount_cents),0) FROM exchange_economy_transfers "
        " WHERE direction='WITHDRAW' AND status IN ('PREPARED','BLOCKED'))-"
        "(SELECT COALESCE(SUM(amount_cents),0) FROM exchange_economy_transfers "
        " WHERE direction='DEPOSIT' AND status IN ('EXTERNAL_APPLIED','COMPLETED'))+"
        "(SELECT COALESCE(SUM(amount_cents),0) FROM exchange_economy_transfers "
        " WHERE direction='WITHDRAW' AND status IN ('EXTERNAL_APPLIED','COMPLETED'))");
    return rows.empty() ? 0 : cellInt64(rows.front(), 0);
}

FrameListing ExchangeService::upsertFrameListing(const FrameListing &listing, const bool administrator) {
    if (listing.target_key.empty() || listing.dimension_name.empty() || listing.seller_uuid.empty() ||
        listing.seller_name.empty() || listing.price_cents <= 0 || listing.item.type.empty()) {
        throw std::runtime_error("frame listing is incomplete");
    }
    ensureAccount(listing.seller_uuid, listing.seller_name);

    Transaction transaction(database_);
    const auto existing = database_.query(std::format(
        "SELECT {} FROM exchange_frame_listing_bindings b "
        "JOIN exchange_frame_listings l ON l.id=b.listing_id WHERE b.target_key={} FOR UPDATE",
        FrameListingColumns, database_.quote(listing.target_key)));
    if (!existing.empty()) {
        const auto current = frameListingFromRow(existing.front());
        if (current.status != FrameListingStatus::Active) {
            throw std::runtime_error("frame listing is already being settled");
        }
        if (!administrator && current.seller_uuid != listing.seller_uuid) {
            throw std::runtime_error("frame listing belongs to another seller");
        }
        database_.execute(std::format(
            "UPDATE exchange_frame_listings SET status='CANCELED',canceled_at=CURRENT_TIMESTAMP(6) WHERE id={}",
            current.id));
        database_.execute(
            std::format("DELETE FROM exchange_frame_listing_bindings WHERE listing_id={}", current.id));
    }

    database_.execute(std::format(
        "INSERT INTO exchange_frame_listings("
        "target_key,dimension_name,block_x,block_y,block_z,seller_uuid,seller_name,price_cents,"
        "item_type,item_data,item_nbt,item_name) VALUES ({},{},{},{},{},{},{},{},{},{},{},{})",
        database_.quote(listing.target_key), database_.quote(listing.dimension_name), listing.block_x,
        listing.block_y, listing.block_z, database_.quote(listing.seller_uuid), database_.quote(listing.seller_name),
        listing.price_cents, database_.quote(listing.item.type), listing.item.data,
        Database::hexLiteral(listing.item.nbt), database_.quote(listing.item.name)));
    auto result = listing;
    result.id = database_.lastInsertId();
    result.status = FrameListingStatus::Active;
    result.buyer_uuid.reset();
    result.buyer_name.reset();
    database_.execute(std::format(
        "INSERT INTO exchange_frame_listing_bindings(target_key,listing_id) VALUES ({},{})",
        database_.quote(result.target_key), result.id));
    transaction.commit();
    return result;
}

std::optional<FrameListing> ExchangeService::findBoundFrameListing(const std::string_view target_key) {
    const auto rows = database_.query(std::format(
        "SELECT {} FROM exchange_frame_listing_bindings b "
        "JOIN exchange_frame_listings l ON l.id=b.listing_id WHERE b.target_key={}",
        FrameListingColumns, database_.quote(target_key)));
    return rows.empty() ? std::nullopt : std::optional<FrameListing>(frameListingFromRow(rows.front()));
}

std::optional<FrameListing> ExchangeService::findFrameListing(const Id listing_id) {
    const auto rows = database_.query(std::format(
        "SELECT {} FROM exchange_frame_listings l WHERE l.id={}", FrameListingColumns, listing_id));
    return rows.empty() ? std::nullopt : std::optional<FrameListing>(frameListingFromRow(rows.front()));
}

std::vector<FrameListing> ExchangeService::boundFrameListings() {
    const auto rows = database_.query(std::format(
        "SELECT {} FROM exchange_frame_listing_bindings b "
        "JOIN exchange_frame_listings l ON l.id=b.listing_id ORDER BY l.id",
        FrameListingColumns));
    std::vector<FrameListing> result;
    result.reserve(rows.size());
    for (const auto &row : rows) {
        result.push_back(frameListingFromRow(row));
    }
    return result;
}

std::vector<FrameListing> ExchangeService::unsettledFrameListings() {
    const auto rows = database_.query(std::format(
        "SELECT {} FROM exchange_frame_listings l WHERE l.status IN ('PAID','DROPPED') ORDER BY l.id",
        FrameListingColumns));
    std::vector<FrameListing> result;
    result.reserve(rows.size());
    for (const auto &row : rows) {
        result.push_back(frameListingFromRow(row));
    }
    return result;
}

void ExchangeService::cancelFrameListing(const Id listing_id, const std::string_view requester_uuid,
                                         const bool administrator) {
    Transaction transaction(database_);
    const auto rows = database_.query(std::format(
        "SELECT seller_uuid,status FROM exchange_frame_listings WHERE id={} FOR UPDATE", listing_id));
    if (rows.empty()) {
        throw std::runtime_error("frame listing does not exist");
    }
    const auto seller_uuid = cellString(rows.front(), 0);
    const auto status = frameListingStatus(cellString(rows.front(), 1));
    if (status == FrameListingStatus::Canceled) {
        transaction.commit();
        return;
    }
    if (status != FrameListingStatus::Active) {
        throw std::runtime_error("paid frame listing cannot be canceled");
    }
    if (!administrator && seller_uuid != requester_uuid) {
        throw std::runtime_error("frame listing belongs to another seller");
    }
    database_.execute(std::format(
        "UPDATE exchange_frame_listings SET status='CANCELED',canceled_at=CURRENT_TIMESTAMP(6) WHERE id={}",
        listing_id));
    database_.execute(
        std::format("DELETE FROM exchange_frame_listing_bindings WHERE listing_id={}", listing_id));
    transaction.commit();
}

FramePurchaseResult ExchangeService::purchaseFrameListing(const Id listing_id,
                                                          const std::string_view buyer_uuid,
                                                          const std::string_view buyer_name,
                                                          const Cents expected_price_cents) {
    if (buyer_uuid.empty() || buyer_name.empty() || expected_price_cents <= 0) {
        throw std::runtime_error("frame purchase identity is incomplete");
    }
    ensureAccount(buyer_uuid, buyer_name);

    Transaction transaction(database_);
    const auto rows = database_.query(std::format(
        "SELECT {} FROM exchange_frame_listings l WHERE l.id={} FOR UPDATE", FrameListingColumns, listing_id));
    if (rows.empty()) {
        throw std::runtime_error("frame listing does not exist");
    }
    auto listing = frameListingFromRow(rows.front());
    if (listing.status != FrameListingStatus::Active) {
        throw std::runtime_error("frame listing is no longer available");
    }
    if (listing.price_cents != expected_price_cents) {
        throw std::runtime_error("frame listing price changed");
    }
    if (listing.seller_uuid == buyer_uuid) {
        throw std::runtime_error("seller cannot buy their own frame listing");
    }
    const auto binding = database_.query(std::format(
        "SELECT target_key FROM exchange_frame_listing_bindings WHERE listing_id={} FOR UPDATE", listing_id));
    if (binding.empty() || cellString(binding.front(), 0) != listing.target_key) {
        throw std::runtime_error("frame listing is no longer bound");
    }

    const auto accounts = database_.query(std::format(
        "SELECT player_uuid FROM exchange_accounts WHERE player_uuid IN ({},{}) ORDER BY player_uuid FOR UPDATE",
        database_.quote(buyer_uuid), database_.quote(listing.seller_uuid)));
    if (accounts.size() != 2) {
        throw std::runtime_error("frame purchase account is missing");
    }

    debit(buyer_uuid, listing.price_cents, "FRAME_PURCHASE", "FRAME_LISTING", listing.id);
    credit(listing.seller_uuid, listing.price_cents, "FRAME_SALE", "FRAME_LISTING", listing.id);
    database_.execute(std::format(
        "UPDATE exchange_frame_listings SET status='PAID',buyer_uuid={},buyer_name={},"
        "paid_at=CURRENT_TIMESTAMP(6) WHERE id={}",
        database_.quote(buyer_uuid), database_.quote(buyer_name), listing.id));
    listing.status = FrameListingStatus::Paid;
    listing.buyer_uuid = std::string(buyer_uuid);
    listing.buyer_name = std::string(buyer_name);
    FramePurchaseResult result{listing, lockedBalance(buyer_uuid), lockedBalance(listing.seller_uuid)};
    transaction.commit();
    return result;
}

void ExchangeService::markFrameListingDropped(const Id listing_id) {
    Transaction transaction(database_);
    const auto rows = database_.query(
        std::format("SELECT status FROM exchange_frame_listings WHERE id={} FOR UPDATE", listing_id));
    if (rows.empty()) {
        throw std::runtime_error("frame listing does not exist");
    }
    const auto status = frameListingStatus(cellString(rows.front(), 0));
    if (status == FrameListingStatus::Dropped || status == FrameListingStatus::Claimed) {
        transaction.commit();
        return;
    }
    if (status != FrameListingStatus::Paid) {
        throw std::runtime_error("unpaid frame listing cannot be dropped");
    }
    database_.execute(std::format(
        "UPDATE exchange_frame_listings SET status='DROPPED',dropped_at=CURRENT_TIMESTAMP(6) WHERE id={}",
        listing_id));
    transaction.commit();
}

void ExchangeService::completeFrameListingPickup(const Id listing_id) {
    Transaction transaction(database_);
    const auto rows = database_.query(
        std::format("SELECT status FROM exchange_frame_listings WHERE id={} FOR UPDATE", listing_id));
    if (rows.empty()) {
        throw std::runtime_error("frame listing does not exist");
    }
    const auto status = frameListingStatus(cellString(rows.front(), 0));
    if (status == FrameListingStatus::Claimed) {
        transaction.commit();
        return;
    }
    if (status != FrameListingStatus::Paid && status != FrameListingStatus::Dropped) {
        throw std::runtime_error("frame listing is not awaiting pickup");
    }
    database_.execute(std::format(
        "UPDATE exchange_frame_listings SET status='CLAIMED',claimed_at=CURRENT_TIMESTAMP(6) WHERE id={}",
        listing_id));
    database_.execute(
        std::format("DELETE FROM exchange_frame_listing_bindings WHERE listing_id={}", listing_id));
    transaction.commit();
}

Market ExchangeService::marketFromRow(const QueryRow &row) {
    Market result;
    result.id = static_cast<Id>(cellInt64(row, 0));
    result.book_id = static_cast<Id>(cellInt64(row, 1));
    result.target_key = cellString(row, 2);
    result.target_kind = cellString(row, 3) == "BLOCK" ? TargetKind::Block : TargetKind::Actor;
    result.dimension_name = cellString(row, 4);
    if (const auto value = optionalInt64(row, 5)) {
        result.block_x = static_cast<int>(*value);
    }
    if (const auto value = optionalInt64(row, 6)) {
        result.block_y = static_cast<int>(*value);
    }
    if (const auto value = optionalInt64(row, 7)) {
        result.block_z = static_cast<int>(*value);
    }
    result.actor_id = optionalInt64(row, 8);
    result.item = {cellString(row, 9), cellInt(row, 10), bytesFromCell(row, 11), cellString(row, 12)};
    result.active = cellInt(row, 13) != 0;
    result.created_by = cellString(row, 14);
    return result;
}

EconomyTransfer ExchangeService::economyTransferFromRow(const QueryRow &row) {
    EconomyTransfer result;
    result.id = static_cast<Id>(cellInt64(row, 0));
    result.operation_key = cellString(row, 1);
    result.player_uuid = cellString(row, 2);
    result.player_name = cellString(row, 3);
    result.direction =
        cellString(row, 4) == "DEPOSIT" ? EconomyTransferDirection::Deposit : EconomyTransferDirection::Withdraw;
    result.amount_cents = cellInt64(row, 5);
    result.amount_units = cellInt64(row, 6);
    result.status = economyTransferStatus(cellString(row, 7));
    result.external_balance_units = optionalInt64(row, 8);
    result.attempt_count = cellInt(row, 9);
    result.last_error = cellString(row, 10);
    return result;
}

FrameListing ExchangeService::frameListingFromRow(const QueryRow &row) {
    FrameListing result;
    result.id = static_cast<Id>(cellInt64(row, 0));
    result.target_key = cellString(row, 1);
    result.dimension_name = cellString(row, 2);
    result.block_x = cellInt(row, 3);
    result.block_y = cellInt(row, 4);
    result.block_z = cellInt(row, 5);
    result.seller_uuid = cellString(row, 6);
    result.seller_name = cellString(row, 7);
    result.price_cents = cellInt64(row, 8);
    result.item = {cellString(row, 9), cellInt(row, 10), bytesFromCell(row, 11), cellString(row, 12)};
    result.status = frameListingStatus(cellString(row, 13));
    result.buyer_uuid = optionalString(row, 14);
    result.buyer_name = optionalString(row, 15);
    return result;
}

Id ExchangeService::lockActiveBook(const Id market_id) {
    const auto identity = database_.query(std::format("SELECT book_id FROM exchange_markets WHERE id={}", market_id));
    if (identity.empty()) {
        throw std::runtime_error("market does not exist");
    }
    const auto book_id = static_cast<Id>(cellInt64(identity.front(), 0));
    if (database_.query(std::format("SELECT id FROM exchange_books WHERE id={} FOR UPDATE", book_id)).empty()) {
        throw std::runtime_error("market order book does not exist");
    }
    const auto market = database_.query(
        std::format("SELECT active,book_id FROM exchange_markets WHERE id={} FOR UPDATE", market_id));
    if (market.empty() || cellInt(market.front(), 0) == 0) {
        throw std::runtime_error("market is not active");
    }
    if (static_cast<Id>(cellInt64(market.front(), 1)) != book_id) {
        throw std::runtime_error("market order-book identity changed unexpectedly");
    }
    return book_id;
}

Cents ExchangeService::lockedBalance(const std::string_view player_uuid) {
    const auto rows = database_.query(std::format(
        "SELECT balance_cents FROM exchange_accounts WHERE player_uuid={} FOR UPDATE", database_.quote(player_uuid)));
    if (rows.empty()) {
        throw std::runtime_error("exchange account does not exist");
    }
    return cellInt64(rows.front(), 0);
}

void ExchangeService::credit(const std::string_view player_uuid, const Cents amount, const std::string_view reason,
                             const std::string_view reference_type, const std::optional<Id> reference_id) {
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
    recordBalanceChange(player_uuid, amount, reason, reference_type, reference_id);
}

void ExchangeService::debit(const std::string_view player_uuid, const Cents amount, const std::string_view reason,
                            const std::string_view reference_type, const std::optional<Id> reference_id) {
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
    recordBalanceChange(player_uuid, -amount, reason, reference_type, reference_id);
}

void ExchangeService::recordBalanceChange(const std::string_view player_uuid, const Cents delta_cents,
                                          const std::string_view reason, const std::string_view reference_type,
                                          const std::optional<Id> reference_id) {
    if (reason.empty()) {
        throw std::runtime_error("balance ledger reason must not be empty");
    }
    const auto sql_reference_type = reference_type.empty() ? std::string("NULL") : database_.quote(reference_type);
    const auto sql_reference_id = reference_id ? std::to_string(*reference_id) : std::string("NULL");
    database_.execute(std::format(
        "INSERT INTO exchange_balance_ledger(player_uuid,delta_cents,reason,reference_type,reference_id) "
        "VALUES ({},{},{},{},{})",
        database_.quote(player_uuid), delta_cents, database_.quote(reason), sql_reference_type, sql_reference_id));
}

void ExchangeService::addDelivery(const std::string_view player_uuid, const Id market_id, const int quantity,
                                  const std::string_view reason) {
    if (quantity <= 0) {
        return;
    }
    database_.execute(
        std::format("INSERT INTO exchange_deliveries(player_uuid,market_id,quantity,reason) VALUES ({},{},{},{})",
                    database_.quote(player_uuid), market_id, quantity, database_.quote(reason)));
}

void ExchangeService::validateRequest(const OrderRequest &request) {
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

} // namespace exchange
