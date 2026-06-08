#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "endstone_exchange/core/Models.hpp"

namespace exchange::ui {

enum class ActionKind {
    Noop,
    OpenDashboard,
    DashboardCategory,
    DashboardProduct,
    DashboardPage,
    DashboardCategoryPage,
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

enum class ControlKind {
    Button,
    Header,
    Label,
    Divider
};

struct ButtonSpec {
    std::string text;
    std::string icon;
    ActionKind action{ActionKind::Back};
    std::string target;
};

struct ControlSpec {
    ControlKind kind{ControlKind::Button};
    std::string text;
    ButtonSpec button;
};

struct FormSpec {
    std::string title;
    std::string body;
    std::vector<ButtonSpec> buttons;
    std::vector<ControlSpec> controls;
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

struct DashboardProductView {
    Product product;
    Quote quote;
    bool selected{false};
};

struct DashboardView {
    std::vector<CategorySpec> categories;
    std::string active_category_id;
    std::string active_category_name;
    std::size_t category_page{0};
    std::size_t total_category_pages{1};
    std::vector<DashboardProductView> products;
    std::size_t page{0};
    std::size_t total_pages{1};
    std::size_t total_products{0};
    std::optional<ProductView> selected_product;
    std::optional<std::int64_t> balance;
    bool admin{false};
    std::string search_query;
};

namespace dashboard_layout {

inline constexpr std::size_t kCategorySlots = 9;
inline constexpr std::size_t kProductSlots = 28;
inline constexpr std::size_t kToolSlots = 7;
inline constexpr std::size_t kTradeSlots = 5;

inline constexpr std::size_t kCategoryStart = 0;
inline constexpr std::size_t kProductStart = kCategoryStart + kCategorySlots;
inline constexpr std::size_t kToolStart = kProductStart + kProductSlots;
inline constexpr std::size_t kTradeStart = kToolStart + kToolSlots;
inline constexpr std::size_t kTotalButtons = kTradeStart + kTradeSlots;

}  // namespace dashboard_layout

namespace chest_layout {

inline constexpr std::size_t kRows = 6;
inline constexpr std::size_t kColumns = 9;
inline constexpr std::size_t kTotalSlots = kRows * kColumns;
inline constexpr std::size_t kCategorySlots = kRows;
inline constexpr std::size_t kProductSlots = 30;

inline constexpr std::size_t slot(std::size_t row, std::size_t column) {
    return row * kColumns + column;
}

inline constexpr std::array<std::size_t, kCategorySlots> kCategoryColumn{
    slot(0, 0), slot(1, 0), slot(2, 0), slot(3, 0), slot(4, 0), slot(5, 0),
};

inline constexpr std::array<std::size_t, kProductSlots> kProductGrid{
    slot(0, 1), slot(0, 2), slot(0, 3), slot(0, 4), slot(0, 5),
    slot(1, 1), slot(1, 2), slot(1, 3), slot(1, 4), slot(1, 5),
    slot(2, 1), slot(2, 2), slot(2, 3), slot(2, 4), slot(2, 5),
    slot(3, 1), slot(3, 2), slot(3, 3), slot(3, 4), slot(3, 5),
    slot(4, 1), slot(4, 2), slot(4, 3), slot(4, 4), slot(4, 5),
    slot(5, 1), slot(5, 2), slot(5, 3), slot(5, 4), slot(5, 5),
};

inline constexpr std::array<std::size_t, kRows> kToolColumn{
    slot(0, 6), slot(1, 6), slot(2, 6), slot(3, 6), slot(4, 6), slot(5, 6),
};

inline constexpr std::size_t kInfoSlot = slot(0, 7);
inline constexpr std::size_t kBookSlot = slot(0, 8);
inline constexpr std::size_t kBuyMarketSlot = slot(1, 7);
inline constexpr std::size_t kBuyLimitSlot = slot(1, 8);
inline constexpr std::size_t kSellMarketSlot = slot(2, 7);
inline constexpr std::size_t kSellLimitSlot = slot(2, 8);
inline constexpr std::size_t kBidSlot = slot(3, 7);
inline constexpr std::size_t kAskSlot = slot(3, 8);
inline constexpr std::size_t kDepthSlot = slot(4, 7);
inline constexpr std::size_t kLastSlot = slot(4, 8);
inline constexpr std::size_t kCloseSlot = slot(5, 7);
inline constexpr std::size_t kAdminSlot = slot(5, 8);

}  // namespace chest_layout

class ExchangeUiModel {
public:
    explicit ExchangeUiModel(std::size_t page_size = 18);

    FormSpec fixedFrame(FormSpec form) const;
    FormSpec dashboard(const DashboardView& view) const;
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
