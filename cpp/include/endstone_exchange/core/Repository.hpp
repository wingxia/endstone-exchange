#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "endstone_exchange/core/Models.hpp"

namespace exchange {

class Repository {
public:
    virtual ~Repository() = default;

    virtual void upsertProduct(const Product& product) = 0;
    virtual std::optional<Product> getProduct(const std::string& product_key) const = 0;
    virtual std::vector<Product> listProducts(const std::optional<std::string>& category = std::nullopt) const = 0;

    virtual Order createOrder(Order order) = 0;
    virtual void updateOrder(const Order& order) = 0;
    virtual std::optional<Order> getOrder(std::int64_t order_id) const = 0;
    virtual std::vector<Order> matchingSellOrders(const std::string& product_key, std::optional<std::int64_t> max_price, std::size_t limit) const = 0;
    virtual std::vector<Order> matchingBuyOrders(const std::string& product_key, std::optional<std::int64_t> min_price, std::size_t limit) const = 0;
    virtual std::vector<Order> playerOrders(const std::string& player_uuid, bool include_closed) const = 0;

    virtual ItemLot createItemLot(ItemLot lot) = 0;
    virtual void updateItemLot(const ItemLot& lot) = 0;
    virtual std::vector<ItemLot> lotsForOrder(std::int64_t order_id) const = 0;

    virtual ReservedFund createReservedFund(ReservedFund fund) = 0;
    virtual void updateReservedFund(const ReservedFund& fund) = 0;
    virtual std::optional<ReservedFund> reservedFundForOrder(std::int64_t order_id) const = 0;

    virtual Trade createTrade(Trade trade) = 0;
    virtual std::vector<Trade> tradesForProduct(const std::string& product_key) const = 0;
    virtual void addTradeItem(std::int64_t trade_id, std::int64_t lot_id, std::int32_t quantity, std::vector<std::uint8_t> nbt_blob, std::string nbt_summary) = 0;

    virtual void addMailboxItem(MailboxItem item) = 0;
    virtual std::vector<MailboxItem> mailboxForPlayer(const std::string& player_uuid) const = 0;

    virtual SettlementJob createSettlementJob(SettlementJob job) = 0;
    virtual void updateSettlementJob(const SettlementJob& job) = 0;
    virtual std::vector<SettlementJob> pendingSettlementJobs(std::size_t limit) const = 0;
};

class InMemoryRepository final : public Repository {
public:
    void upsertProduct(const Product& product) override;
    std::optional<Product> getProduct(const std::string& product_key) const override;
    std::vector<Product> listProducts(const std::optional<std::string>& category = std::nullopt) const override;

    Order createOrder(Order order) override;
    void updateOrder(const Order& order) override;
    std::optional<Order> getOrder(std::int64_t order_id) const override;
    std::vector<Order> matchingSellOrders(const std::string& product_key, std::optional<std::int64_t> max_price, std::size_t limit) const override;
    std::vector<Order> matchingBuyOrders(const std::string& product_key, std::optional<std::int64_t> min_price, std::size_t limit) const override;
    std::vector<Order> playerOrders(const std::string& player_uuid, bool include_closed) const override;

    ItemLot createItemLot(ItemLot lot) override;
    void updateItemLot(const ItemLot& lot) override;
    std::vector<ItemLot> lotsForOrder(std::int64_t order_id) const override;

    ReservedFund createReservedFund(ReservedFund fund) override;
    void updateReservedFund(const ReservedFund& fund) override;
    std::optional<ReservedFund> reservedFundForOrder(std::int64_t order_id) const override;

    Trade createTrade(Trade trade) override;
    std::vector<Trade> tradesForProduct(const std::string& product_key) const override;
    void addTradeItem(std::int64_t trade_id, std::int64_t lot_id, std::int32_t quantity, std::vector<std::uint8_t> nbt_blob, std::string nbt_summary) override;

    void addMailboxItem(MailboxItem item) override;
    std::vector<MailboxItem> mailboxForPlayer(const std::string& player_uuid) const override;

    SettlementJob createSettlementJob(SettlementJob job) override;
    void updateSettlementJob(const SettlementJob& job) override;
    std::vector<SettlementJob> pendingSettlementJobs(std::size_t limit) const override;

    std::vector<Order> orders_;
    std::vector<ItemLot> lots_;
    std::vector<ReservedFund> reserved_funds_;
    std::vector<Trade> trades_;
    std::vector<MailboxItem> mailbox_;
    std::vector<SettlementJob> jobs_;

private:
    std::vector<Product> products_;
    std::int64_t next_order_id_{1};
    std::int64_t next_lot_id_{1};
    std::int64_t next_fund_id_{1};
    std::int64_t next_trade_id_{1};
    std::int64_t next_mailbox_id_{1};
    std::int64_t next_job_id_{1};
};

bool isOpen(OrderStatus status);

}  // namespace exchange

