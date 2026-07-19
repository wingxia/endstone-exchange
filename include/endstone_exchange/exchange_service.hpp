#pragma once

#include "endstone_exchange/database.hpp"
#include "endstone_exchange/domain.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace exchange {

class ExchangeService {
  public:
    ExchangeService(Database &database, Cents initial_balance_cents, int max_order_quantity);

    void ensureAccount(std::string_view player_uuid, std::string_view player_name);
    [[nodiscard]] Cents balance(std::string_view player_uuid);
    Cents addBalance(std::string_view player_uuid, std::string_view player_name, Cents delta_cents);

    [[nodiscard]] Market activateMarket(const Market &market);
    void closeMarket(Id market_id);
    void retireMarket(Id market_id);
    [[nodiscard]] std::optional<Market> findMarketByTarget(std::string_view target_key);
    [[nodiscard]] std::optional<Market> findMarket(Id market_id);
    [[nodiscard]] std::vector<Market> activeMarkets();

    [[nodiscard]] OrderBook orderBook(Id market_id, int depth);
    [[nodiscard]] std::unordered_map<Id, OrderBook> topOfBooks(const std::vector<Id> &market_ids);
    [[nodiscard]] ExecutionResult placeOrder(const OrderRequest &request);
    void cancelOrder(Id order_id, std::string_view player_uuid, bool administrator = false);
    [[nodiscard]] std::vector<OpenOrder> openOrders(std::string_view player_uuid);

    [[nodiscard]] std::vector<Delivery> pendingDeliveries(std::string_view player_uuid);
    [[nodiscard]] std::vector<DeliveryClaim> unfinishedDeliveryClaims(std::string_view player_uuid);
    [[nodiscard]] std::optional<DeliveryClaim> findDeliveryClaim(Id claim_id, std::string_view player_uuid);
    [[nodiscard]] DeliveryClaim prepareDeliveryClaim(Id delivery_id, std::string_view player_uuid, int quantity);
    void completeDeliveryClaim(Id claim_id, int delivered_quantity);
    void markDeliveryClaimCleaned(Id claim_id);

    [[nodiscard]] SellEscrow prepareSellEscrow(const OrderRequest &request);
    void markSellEscrowTagged(Id escrow_id, int tagged_quantity, int receipt_count);
    [[nodiscard]] ExecutionResult executeSellEscrow(Id escrow_id);
    void cancelSellEscrow(Id escrow_id);
    void markSellEscrowCleaned(Id escrow_id);
    [[nodiscard]] std::vector<SellEscrow> unfinishedSellEscrows(std::string_view player_uuid);
    [[nodiscard]] std::optional<SellEscrow> findSellEscrow(Id escrow_id, std::string_view player_uuid);

  private:
    Database &database_;
    Cents initial_balance_cents_;
    int max_order_quantity_;

    [[nodiscard]] static Market marketFromRow(const QueryRow &row);
    [[nodiscard]] Id lockActiveBook(Id market_id);
    [[nodiscard]] Cents lockedBalance(std::string_view player_uuid);
    void credit(std::string_view player_uuid, Cents amount, std::string_view reason,
                std::string_view reference_type = {}, std::optional<Id> reference_id = std::nullopt);
    void debit(std::string_view player_uuid, Cents amount, std::string_view reason,
               std::string_view reference_type = {}, std::optional<Id> reference_id = std::nullopt);
    void recordBalanceChange(std::string_view player_uuid, Cents delta_cents, std::string_view reason,
                             std::string_view reference_type, std::optional<Id> reference_id);
    void addDelivery(std::string_view player_uuid, Id market_id, int quantity, std::string_view reason);
    [[nodiscard]] ExecutionResult placeBuy(const OrderRequest &request);
    [[nodiscard]] ExecutionResult placeSell(const OrderRequest &request);
    [[nodiscard]] ExecutionResult placeSellLocked(const OrderRequest &request);
    void validateRequest(const OrderRequest &request);
};

} // namespace exchange
