#pragma once

#include "endstone_exchange/database.hpp"
#include "endstone_exchange/domain.hpp"

#include <optional>
#include <string>
#include <vector>

namespace exchange {

class ExchangeService {
public:
    ExchangeService(Database &database, Cents initial_balance_cents, int max_order_quantity);

    void ensureAccount(std::string_view player_uuid, std::string_view player_name);
    [[nodiscard]] Cents balance(std::string_view player_uuid);
    Cents addBalance(std::string_view player_uuid, std::string_view player_name, Cents delta_cents);

    [[nodiscard]] Market activateMarket(const Market &market);
    void deactivateMarket(Id market_id);
    [[nodiscard]] std::optional<Market> findMarketByTarget(std::string_view target_key);
    [[nodiscard]] std::optional<Market> findMarket(Id market_id);
    [[nodiscard]] std::vector<Market> activeMarkets();

    [[nodiscard]] OrderBook orderBook(Id market_id, int depth);
    [[nodiscard]] ExecutionResult placeOrder(const OrderRequest &request);
    void cancelOrder(Id order_id, std::string_view player_uuid, bool administrator = false);
    [[nodiscard]] std::vector<OpenOrder> openOrders(std::string_view player_uuid);

    [[nodiscard]] std::vector<Delivery> pendingDeliveries(std::string_view player_uuid);
    void markDeliveryClaimed(Id delivery_id, int quantity);

private:
    Database &database_;
    Cents initial_balance_cents_;
    int max_order_quantity_;

    [[nodiscard]] static Market marketFromRow(const QueryRow &row);
    [[nodiscard]] Cents lockedBalance(std::string_view player_uuid);
    void credit(std::string_view player_uuid, Cents amount);
    void debit(std::string_view player_uuid, Cents amount);
    void addDelivery(std::string_view player_uuid, Id market_id, int quantity, std::string_view reason);
    [[nodiscard]] ExecutionResult placeBuy(const OrderRequest &request);
    [[nodiscard]] ExecutionResult placeSell(const OrderRequest &request);
    void validateRequest(const OrderRequest &request);
};

}  // namespace exchange
