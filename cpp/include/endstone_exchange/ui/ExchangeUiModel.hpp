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
    OpenInventorySell,
    SellAll,
    OpenMyOrders,
    OpenMailbox,
    OpenAdmin,
    AdminRestock,
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

    FormSpec home(const std::vector<CategorySpec>& categories, bool admin) const;
    FormSpec categoryPage(const CategorySpec& category, const std::vector<Product>& products, std::size_t page) const;
    FormSpec productPage(const ProductView& view) const;
    FormSpec orderBookPage(const Product& product, const OrderBook& book) const;
    FormSpec adminHome(const std::vector<Product>& stock_templates) const;

private:
    std::size_t page_size_;
};

}  // namespace exchange::ui

