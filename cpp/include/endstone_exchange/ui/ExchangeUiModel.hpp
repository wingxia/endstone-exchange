#pragma once

#include <optional>
#include <string>
#include <vector>

#include "endstone_exchange/core/Models.hpp"

namespace exchange::ui {

enum class ActionKind {
    OpenCategory,
    OpenProduct,
    MarketBuy,
    LimitBuy,
    MarketSell,
    LimitSell,
    OpenOrderBook,
    OpenSearch,
    OpenAllProducts,
    OpenInventorySell,
    SellAll,
    OpenMyOrders,
    OpenMailbox,
    OpenAdmin,
    AdminRestock,
    ConfirmTrade,
    CancelOrder,
    ClaimMailboxItem,
    ClaimAllMailbox,
    Back,
    NextPage,
    PrevPage
};

struct ButtonSpec {
    std::string text;
    std::string icon;
    ActionKind action{ActionKind::Back};
    std::string target;
};

struct FormSpec {
    std::string title;
    std::string body;
    std::vector<ButtonSpec> buttons;
};

struct CategorySpec {
    std::string id;
    std::string name;
    std::string icon;
};

struct ProductView {
    Product product;
    Quote quote;
};

class ExchangeUiModel {
public:
    explicit ExchangeUiModel(std::size_t page_size = 18);

    FormSpec home(const std::vector<CategorySpec>& categories, bool admin, std::optional<std::int64_t> balance = std::nullopt) const;
    FormSpec categoryPage(const CategorySpec& category, const std::vector<Product>& products, std::size_t page) const;
    FormSpec searchResults(const std::string& query, const std::vector<Product>& products, std::size_t page) const;
    FormSpec productPage(const ProductView& view) const;
    FormSpec orderBookPage(const Product& product, const OrderBook& book) const;
    FormSpec myOrdersPage(const std::vector<Order>& orders) const;
    FormSpec mailboxPage(const std::vector<MailboxItem>& items) const;
    FormSpec adminHome(const std::vector<Product>& stock_templates) const;

private:
    FormSpec productListPage(std::string title, std::string body, std::string page_target, const std::vector<Product>& products, std::size_t page, ActionKind page_action) const;
    std::size_t page_size_;
};

}  // namespace exchange::ui
