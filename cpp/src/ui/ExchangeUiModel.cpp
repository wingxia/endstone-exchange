#include "endstone_exchange/ui/ExchangeUiModel.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace exchange::ui {
namespace {

std::string money(std::optional<std::int64_t> value) {
    return value ? std::to_string(*value) : "-";
}

std::string spread(const Quote& quote) {
    if (!quote.best_bid || !quote.best_ask) {
        return "-";
    }
    return std::to_string(*quote.best_ask - *quote.best_bid);
}

std::string pageTarget(const std::string& base, std::size_t page) {
    return base + "|" + std::to_string(page);
}

std::string side(OrderSide side) {
    return side == OrderSide::Buy ? "买单" : "卖单";
}

std::string type(OrderType type) {
    return type == OrderType::Market ? "市价" : "限价";
}

std::string status(OrderStatus status) {
    switch (status) {
    case OrderStatus::Open:
        return "开放";
    case OrderStatus::Partial:
        return "部分成交";
    case OrderStatus::Filled:
        return "已成交";
    case OrderStatus::Cancelled:
        return "已取消";
    case OrderStatus::Expired:
        return "已过期";
    case OrderStatus::Failed:
        return "失败";
    }
    return "-";
}

bool openStatus(OrderStatus status) {
    return status == OrderStatus::Open || status == OrderStatus::Partial;
}

void addButton(FormSpec& form, ButtonSpec button) {
    form.buttons.push_back(button);
    form.controls.push_back({ControlKind::Button, "", std::move(button)});
}

void addHeader(FormSpec& form, std::string text) {
    form.controls.push_back({ControlKind::Header, std::move(text), {}});
}

void addLabel(FormSpec& form, std::string text) {
    form.controls.push_back({ControlKind::Label, std::move(text), {}});
}

void addDivider(FormSpec& form) {
    form.controls.push_back({ControlKind::Divider, "", {}});
}

std::string productQuoteLine(const Quote& quote) {
    std::ostringstream out;
    out << "买 " << money(quote.best_bid) << " / 卖 " << money(quote.best_ask)
        << " / 成交 " << money(quote.last_price);
    return out.str();
}

ButtonSpec emptySlot() {
    return {"", "", ActionKind::Noop, ""};
}

std::string compactText(const std::string& value, std::size_t max_chars) {
    if (value.size() <= max_chars) {
        return value;
    }
    if (max_chars <= 3) {
        return value.substr(0, max_chars);
    }
    return value.substr(0, max_chars - 3) + "...";
}

bool tradePanelAction(ActionKind action) {
    switch (action) {
    case ActionKind::MarketBuy:
    case ActionKind::LimitBuy:
    case ActionKind::MarketSell:
    case ActionKind::LimitSell:
    case ActionKind::OpenOrderBook:
    case ActionKind::ConfirmTrade:
        return true;
    default:
        return false;
    }
}

bool toolPanelAction(ActionKind action) {
    switch (action) {
    case ActionKind::DashboardPage:
    case ActionKind::DashboardCategoryPage:
    case ActionKind::OpenDashboard:
    case ActionKind::OpenSearch:
    case ActionKind::OpenAllProducts:
    case ActionKind::OpenMyOrders:
    case ActionKind::OpenMailbox:
    case ActionKind::OpenAdmin:
    case ActionKind::NextPage:
    case ActionKind::PrevPage:
        return true;
    default:
        return false;
    }
}

}  // namespace

ExchangeUiModel::ExchangeUiModel(std::size_t page_size) : page_size_(std::max<std::size_t>(6, page_size)) {}

FormSpec ExchangeUiModel::fixedFrame(FormSpec form) const {
    if (form.buttons.size() == dashboard_layout::kTotalButtons && form.controls.size() == dashboard_layout::kTotalButtons) {
        return form;
    }

    std::vector<ButtonSpec> original = form.buttons;
    if (original.empty() && !form.controls.empty()) {
        for (const auto& control : form.controls) {
            if (control.kind == ControlKind::Button) {
                original.push_back(control.button);
            }
        }
    }

    form.buttons.assign(dashboard_layout::kTotalButtons, emptySlot());
    form.controls.clear();

    std::optional<ButtonSpec> back_button;
    std::size_t product_slot = 0;
    std::size_t tool_slot = 0;
    std::size_t trade_slot = 0;
    const bool has_back = std::any_of(original.begin(), original.end(), [](const ButtonSpec& button) {
        return button.action == ActionKind::Back;
    });
    const auto max_trade_slots = has_back ? dashboard_layout::kTradeSlots - 1 : dashboard_layout::kTradeSlots;

    const auto place = [&](std::size_t index, ButtonSpec button) {
        if (index < form.buttons.size()) {
            form.buttons[index] = std::move(button);
        }
    };

    for (auto button : original) {
        if (button.action == ActionKind::Back) {
            back_button = std::move(button);
            continue;
        }
        if (tradePanelAction(button.action) && trade_slot < max_trade_slots) {
            place(dashboard_layout::kTradeStart + trade_slot, std::move(button));
            ++trade_slot;
            continue;
        }
        if (toolPanelAction(button.action) && tool_slot < dashboard_layout::kToolSlots) {
            place(dashboard_layout::kToolStart + tool_slot, std::move(button));
            ++tool_slot;
            continue;
        }
        if (product_slot < dashboard_layout::kProductSlots) {
            place(dashboard_layout::kProductStart + product_slot, std::move(button));
            ++product_slot;
        }
    }

    if (back_button) {
        place(dashboard_layout::kTradeStart + dashboard_layout::kTradeSlots - 1, *back_button);
    }
    for (const auto& button : form.buttons) {
        form.controls.push_back({ControlKind::Button, "", button});
    }
    return form;
}

FormSpec ExchangeUiModel::dashboard(const DashboardView& view) const {
    FormSpec form;
    form.title = "Exchange Chest UI";
    std::ostringstream body;
    body << "Exchange | 箱子式交易面板"
         << "\n余额: " << money(view.balance)
         << "\n分类: " << (view.active_category_name.empty() ? "-" : view.active_category_name)
         << "\n分类组: " << (view.category_page + 1) << " / " << std::max<std::size_t>(1, view.total_category_pages)
         << "\n商品页: " << (view.page + 1) << " / " << std::max<std::size_t>(1, view.total_pages)
         << "\n结果数: " << view.total_products;
    if (!view.search_query.empty()) {
        body << "\n搜索: " << view.search_query;
    }
    if (view.selected_product) {
        const auto& selected = *view.selected_product;
        body << "\n\n当前商品"
             << "\n商品: " << selected.product.display_name
             << "\n物品ID: " << selected.product.item_id
             << "\n最高买价: " << money(selected.quote.best_bid)
             << "\n最低卖价: " << money(selected.quote.best_ask)
             << "\n差价: " << spread(selected.quote)
             << "\n买方需求: " << selected.quote.bid_quantity
             << "\n卖方库存: " << selected.quote.ask_quantity
             << "\n最近成交: " << money(selected.quote.last_price);
    } else {
        body << "\n当前分类没有可交易商品。";
    }
    form.body = body.str();

    const auto category_page_count = std::max<std::size_t>(1, view.total_category_pages);
    const auto category_nav_target = view.category_page + 1 < category_page_count ? view.category_page + 1 : 0;
    for (std::size_t slot = 0; slot < dashboard_layout::kCategorySlots; ++slot) {
        if (slot < view.categories.size()) {
            const auto& category = view.categories[slot];
            const auto selected = view.search_query.empty() && category.id == view.active_category_id;
            addButton(form, {(selected ? "当前 " : "") + compactText(category.name, 12), category.icon, ActionKind::DashboardCategory, category.id});
        } else if (slot + 1 == dashboard_layout::kCategorySlots && category_page_count > 1) {
            addButton(form, {"更多 " + std::to_string(category_nav_target + 1) + "/" + std::to_string(category_page_count),
                             "textures/ui/arrow_right", ActionKind::DashboardCategoryPage, std::to_string(category_nav_target)});
        } else {
            addButton(form, emptySlot());
        }
    }

    for (std::size_t slot = 0; slot < dashboard_layout::kProductSlots; ++slot) {
        if (slot >= view.products.size()) {
            addButton(form, emptySlot());
            continue;
        }
        const auto& product_view = view.products[slot];
        std::ostringstream label;
        if (product_view.selected) {
            label << "选 ";
        }
        label << compactText(product_view.product.display_name, 14);
        addButton(form, {label.str(), product_view.product.icon, ActionKind::DashboardProduct, product_view.product.product_key});
    }

    if (view.page > 0) {
        addButton(form, {"上页", "textures/ui/arrow_left", ActionKind::DashboardPage, std::to_string(view.page - 1)});
    } else {
        addButton(form, {"上页", "textures/ui/arrow_left", ActionKind::Noop, ""});
    }
    if (view.page + 1 < view.total_pages) {
        addButton(form, {"下页", "textures/ui/arrow_right", ActionKind::DashboardPage, std::to_string(view.page + 1)});
    } else {
        addButton(form, {"下页", "textures/ui/arrow_right", ActionKind::Noop, ""});
    }
    addButton(form, {"搜索", "textures/ui/magnifyingGlass", ActionKind::OpenSearch, ""});
    addButton(form, {"全部", "textures/items/book_normal", ActionKind::OpenAllProducts, ""});
    addButton(form, {"订单", "textures/items/paper", ActionKind::OpenMyOrders, ""});
    addButton(form, {"邮箱", "textures/items/minecart_chest", ActionKind::OpenMailbox, ""});
    if (view.admin) {
        addButton(form, {"管理", "textures/items/gold_ingot", ActionKind::OpenAdmin, ""});
    } else {
        addButton(form, {"管理", "textures/items/gold_ingot", ActionKind::Noop, ""});
    }

    const auto selected_key = view.selected_product ? view.selected_product->product.product_key : std::string{};
    if (view.selected_product) {
        addButton(form, {"买市", "textures/items/emerald", ActionKind::MarketBuy, selected_key});
        addButton(form, {"买挂", "textures/items/paper", ActionKind::LimitBuy, selected_key});
        addButton(form, {"卖市", "textures/items/chest", ActionKind::MarketSell, selected_key});
        addButton(form, {"卖挂", "textures/items/writable_book", ActionKind::LimitSell, selected_key});
        addButton(form, {"簿", "textures/items/book_normal", ActionKind::OpenOrderBook, selected_key});
    } else {
        addButton(form, {"买市", "textures/items/emerald", ActionKind::Noop, ""});
        addButton(form, {"买挂", "textures/items/paper", ActionKind::Noop, ""});
        addButton(form, {"卖市", "textures/items/chest", ActionKind::Noop, ""});
        addButton(form, {"卖挂", "textures/items/writable_book", ActionKind::Noop, ""});
        addButton(form, {"簿", "textures/items/book_normal", ActionKind::Noop, ""});
    }

    while (form.buttons.size() < dashboard_layout::kTotalButtons) {
        addButton(form, emptySlot());
    }
    if (form.buttons.size() > dashboard_layout::kTotalButtons) {
        form.buttons.resize(dashboard_layout::kTotalButtons);
        form.controls.resize(dashboard_layout::kTotalButtons);
    }
    return form;
}

FormSpec ExchangeUiModel::home(const std::vector<CategorySpec>& categories, bool admin, std::optional<std::int64_t> balance) const {
    FormSpec form;
    form.title = "UMoney Exchange | 选择区";
    std::ostringstream body;
    body << "余额: " << money(balance)
         << "\n\n左侧 | 物品选择区"
         << "\n先按分类浏览全物品图标，选中商品后进入右侧下单区。"
         << "\n底部工具可搜索、查看订单或领取邮箱。";
    form.body = body.str();
    for (const auto& category : categories) {
        form.buttons.push_back({"分类 | " + category.name, category.icon, ActionKind::OpenCategory, category.id});
    }
    form.buttons.push_back({"工具 | 搜索物品", "textures/ui/magnifyingGlass", ActionKind::OpenSearch, ""});
    form.buttons.push_back({"工具 | 全部物品", "textures/items/book_normal", ActionKind::OpenAllProducts, ""});
    form.buttons.push_back({"账户 | 我的订单", "textures/items/paper", ActionKind::OpenMyOrders, ""});
    form.buttons.push_back({"账户 | 邮箱领取", "textures/items/minecart_chest", ActionKind::OpenMailbox, ""});
    if (admin) {
        form.buttons.push_back({"账户 | 管理员", "textures/items/gold_ingot", ActionKind::OpenAdmin, ""});
    }
    return form;
}

FormSpec ExchangeUiModel::categoryPage(const CategorySpec& category, const std::vector<Product>& products, std::size_t page) const {
    return productListPage(category.name + " | 选择区", "左侧 | 分类物品\n点击带图标商品进入右侧下单区。", category.id, products, page, ActionKind::OpenCategory);
}

FormSpec ExchangeUiModel::searchResults(const std::string& query, const std::vector<Product>& products, std::size_t page) const {
    const auto page_target = query == "全部物品" ? "all" : "search";
    const auto title = query == "全部物品" ? "全部物品 | 选择区" : "搜索: " + query + " | 选择区";
    return productListPage(title, "左侧 | 物品选择\n结果按分类和名称排序，点击商品进入右侧下单区。", page_target, products, page, ActionKind::OpenSearch);
}

FormSpec ExchangeUiModel::productListPage(std::string title, std::string body, std::string page_target, const std::vector<Product>& products, std::size_t page, ActionKind) const {
    FormSpec form;
    form.title = std::move(title);
    std::ostringstream content;
    content << body << "\n当前页: " << (page + 1) << " / " << std::max<std::size_t>(1, (products.size() + page_size_ - 1) / page_size_)
            << "\n结果数: " << products.size();
    form.body = content.str();
    const auto start = page * page_size_;
    const auto end = std::min(products.size(), start + page_size_);
    if (page > 0) {
        form.buttons.push_back({"上页", "textures/ui/arrow_left", ActionKind::PrevPage, pageTarget(page_target, page - 1)});
    }
    for (std::size_t i = start; i < end; ++i) {
        const auto& product = products[i];
        form.buttons.push_back({compactText(product.display_name, 14), product.icon, ActionKind::OpenProduct, product.product_key});
    }
    if (end < products.size()) {
        form.buttons.push_back({"下页", "textures/ui/arrow_right", ActionKind::NextPage, pageTarget(page_target, page + 1)});
    }
    form.buttons.push_back({"返", "textures/ui/undoArrow", ActionKind::Back, ""});
    return form;
}

FormSpec ExchangeUiModel::productPage(const ProductView& view) const {
    FormSpec form;
    form.title = view.product.display_name + " | 下单区";
    std::ostringstream body;
    body << "右侧 | 行情与下单"
         << "\n商品: " << view.product.display_name
         << "\n物品ID: " << view.product.item_id
         << "\n\n行情"
         << "\n最高买价: " << money(view.quote.best_bid)
         << "\n最低卖价: " << money(view.quote.best_ask)
         << "\n差价: " << spread(view.quote)
         << "\n买方需求: " << view.quote.bid_quantity
         << "\n卖方库存: " << view.quote.ask_quantity
         << "\n最近成交: " << money(view.quote.last_price)
         << "\n\n下单"
         << "\n选择买入/卖出和市价/挂单快捷入口。";
    form.body = body.str();
    form.buttons.push_back({"买市", "textures/items/emerald", ActionKind::MarketBuy, view.product.product_key});
    form.buttons.push_back({"买挂", "textures/items/paper", ActionKind::LimitBuy, view.product.product_key});
    form.buttons.push_back({"卖市", "textures/items/chest", ActionKind::MarketSell, view.product.product_key});
    form.buttons.push_back({"卖挂", "textures/items/writable_book", ActionKind::LimitSell, view.product.product_key});
    form.buttons.push_back({"簿", "textures/items/book_normal", ActionKind::OpenOrderBook, view.product.product_key});
    form.buttons.push_back({"返", "textures/ui/undoArrow", ActionKind::Back, ""});
    return form;
}

FormSpec ExchangeUiModel::orderBookPage(const Product& product, const OrderBook& book) const {
    FormSpec form;
    form.title = product.display_name + " 订单簿";
    std::ostringstream body;
    body << "买单按价格高到低，卖单按价格低到高。\n\n买单:\n";
    for (const auto& bid : book.bids) {
        body << "#" << bid.id << " " << bid.player_name << " x" << bid.remaining_quantity << " @ " << *bid.unit_price << "\n";
    }
    body << "\n卖单:\n";
    for (const auto& ask : book.asks) {
        body << "#" << ask.id << " " << ask.player_name << " x" << ask.remaining_quantity << " @ " << *ask.unit_price << "\n";
    }
    form.body = body.str();
    for (const auto& ask : book.asks) {
        form.buttons.push_back({"买#" + std::to_string(ask.id), "textures/items/emerald", ActionKind::MarketBuy, std::to_string(ask.id)});
    }
    for (const auto& bid : book.bids) {
        form.buttons.push_back({"卖#" + std::to_string(bid.id), "textures/items/chest", ActionKind::MarketSell, std::to_string(bid.id)});
    }
    form.buttons.push_back({"返", "textures/ui/undoArrow", ActionKind::Back, product.product_key});
    return form;
}

FormSpec ExchangeUiModel::myOrdersPage(const std::vector<Order>& orders) const {
    FormSpec form;
    form.title = "我的订单";
    std::ostringstream body;
    body << "最近订单: " << orders.size();
    form.body = body.str();
    for (const auto& order : orders) {
        std::ostringstream label;
        label << "#" << order.id << " " << side(order.side) << " " << type(order.type)
              << " x" << order.remaining_quantity << "/" << order.original_quantity
              << " @ " << money(order.unit_price)
              << " " << status(order.status);
        if (openStatus(order.status)) {
            form.buttons.push_back({compactText(label.str(), 18), "textures/ui/trash", ActionKind::CancelOrder, std::to_string(order.id)});
        } else {
            form.buttons.push_back({compactText(label.str(), 18), "textures/items/paper", ActionKind::OpenProduct, order.product_key});
        }
    }
    form.buttons.push_back({"返", "textures/ui/undoArrow", ActionKind::Back, ""});
    return form;
}

FormSpec ExchangeUiModel::mailboxPage(const std::vector<MailboxItem>& items) const {
    FormSpec form;
    form.title = "交易所邮箱";
    form.body = "成交和取消返还的物品会进入这里。";
    if (!items.empty()) {
        form.buttons.push_back({"全领", "textures/items/minecart_chest", ActionKind::ClaimAllMailbox, ""});
    }
    for (const auto& item : items) {
        std::ostringstream label;
        label << "#" << item.id << " x" << item.quantity << " " << item.product_key;
        form.buttons.push_back({compactText(label.str(), 18), "textures/items/chest", ActionKind::ClaimMailboxItem, std::to_string(item.id)});
    }
    form.buttons.push_back({"返", "textures/ui/undoArrow", ActionKind::Back, ""});
    return form;
}

FormSpec ExchangeUiModel::adminHome(const std::vector<Product>& stock_templates) const {
    FormSpec form;
    form.title = "交易所管理员";
    form.body = "为基建材料创建系统卖单，系统卖单成交收入只进入审计流水。";
    for (const auto& product : stock_templates) {
        form.buttons.push_back({"补 " + compactText(product.display_name, 14), product.icon, ActionKind::AdminRestock, product.product_key});
    }
    form.buttons.push_back({"返", "textures/ui/undoArrow", ActionKind::Back, ""});
    return form;
}

}  // namespace exchange::ui
