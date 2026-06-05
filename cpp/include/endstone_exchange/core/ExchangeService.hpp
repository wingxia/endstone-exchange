#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "endstone_exchange/core/Economy.hpp"
#include "endstone_exchange/core/Models.hpp"
#include "endstone_exchange/core/Repository.hpp"

namespace exchange {

class ExchangeService {
public:
    ExchangeService(Repository& repository, Economy& economy);

    std::vector<Product> getCatalog(const std::optional<std::string>& category = std::nullopt) const;
    Quote getQuote(const std::string& product_key) const;
    OrderBook listOrderBook(const std::string& product_key, std::size_t depth) const;
    std::vector<Order> listPlayerOrders(const PlayerRef& player, bool include_closed) const;

    Order placeLimitBuy(const PlayerRef& player, const std::string& product_key, std::int32_t quantity, std::int64_t unit_price);
    Order placeMarketBuy(const PlayerRef& player, const std::string& product_key, std::int32_t quantity);
    Order placeLimitSell(const PlayerRef& player, const std::string& product_key, std::vector<ItemSnapshot> snapshots, std::int64_t unit_price);
    Order placeMarketSell(const PlayerRef& player, const std::string& product_key, std::vector<ItemSnapshot> snapshots);
    Order cancelOrder(const PlayerRef& player, std::int64_t order_id, bool admin = false);

    Order adminCreateSystemSellOrder(const PlayerRef& admin, const std::string& product_key, std::int32_t quantity, std::int64_t unit_price, std::vector<std::uint8_t> nbt_template = {});
    Order adminCancelSystemOrder(const PlayerRef& admin, std::int64_t order_id);

    void processSettlements(std::size_t limit = 50);

private:
    void validateProduct(const std::string& product_key) const;
    void validateQuantity(std::int32_t quantity) const;
    void validatePrice(std::int64_t unit_price) const;
    void validateSnapshots(const std::string& product_key, const std::vector<ItemSnapshot>& snapshots) const;

    void matchBuyOrder(Order& buy_order);
    void matchSellOrder(Order& sell_order);
    Trade executeTrade(Order& buy_order, Order& sell_order, std::int64_t unit_price, std::int32_t quantity);
    void deliverLots(const Order& sell_order, const Order& buy_order, const Trade& trade, std::int32_t quantity);
    void consumeBuyReservation(Order& buy_order, std::int64_t execution_price, std::int32_t quantity, std::int64_t trade_id);
    void refundRemainingReservation(Order& order, const std::string& reason);
    void returnRemainingLots(Order& order, OrderStatus status);
    std::int32_t availableLotQuantity(std::int64_t order_id) const;
    std::pair<std::int32_t, std::int64_t> estimateMarketBuy(const std::string& product_key, std::int32_t quantity) const;
    std::pair<std::int32_t, std::int64_t> estimateMarketSell(const std::string& product_key, std::int32_t quantity) const;
    static OrderStatus statusFor(const Order& order);

    Repository& repository_;
    Economy& economy_;
};

}  // namespace exchange

