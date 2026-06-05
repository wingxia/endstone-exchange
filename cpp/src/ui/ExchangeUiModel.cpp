#include "endstone_exchange/ui/ExchangeUiModel.hpp"

#include <algorithm>
#include <sstream>

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

}  // namespace

ExchangeUiModel::ExchangeUiModel(std::size_t page_size) : page_size_(std::max<std::size_t>(6, page_size)) {}

FormSpec ExchangeUiModel::home(const std::vector<CategorySpec>& categories, bool admin, std::optional<std::int64_t> balance) const {
    FormSpec form;
    form.title = "UMoney Exchange";
    std::ostringstream body;
    body << "余额: " << money(balance)
         << "\n选择左侧分类或搜索物品，再在商品面板提交买入、卖出或挂单。";
    form.body = body.str();
    form.buttons.push_back({"搜索物品", "textures/ui/magnifyingGlass", ActionKind::OpenSearch, ""});
    form.buttons.push_back({"全部物品", "textures/items/book_normal", ActionKind::OpenAllProducts, ""});
    form.buttons.push_back({"我的订单", "textures/items/paper", ActionKind::OpenMyOrders, ""});
    form.buttons.push_back({"邮箱领取", "textures/items/minecart_chest", ActionKind::OpenMailbox, ""});
    for (const auto& category : categories) {
        form.buttons.push_back({category.name, category.icon, ActionKind::OpenCategory, category.id});
    }
    if (admin) {
        form.buttons.push_back({"管理员", "textures/items/gold_ingot", ActionKind::OpenAdmin, ""});
    }
    return form;
}

FormSpec ExchangeUiModel::categoryPage(const CategorySpec& category, const std::vector<Product>& products, std::size_t page) const {
    return productListPage(category.name, "左侧选择物品，右侧商品页提交交易。", category.id, products, page, ActionKind::OpenCategory);
}

FormSpec ExchangeUiModel::searchResults(const std::string& query, const std::vector<Product>& products, std::size_t page) const {
    const auto page_target = query == "全部物品" ? "all" : "search";
    return productListPage("搜索: " + query, "搜索结果按分类和名称排序。", page_target, products, page, ActionKind::OpenSearch);
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
        form.buttons.push_back({"上一页", "textures/ui/arrow_left", ActionKind::PrevPage, pageTarget(page_target, page - 1)});
    }
    for (std::size_t i = start; i < end; ++i) {
        const auto& product = products[i];
        form.buttons.push_back({product.display_name + "\n" + product.item_id, product.icon, ActionKind::OpenProduct, product.product_key});
    }
    if (end < products.size()) {
        form.buttons.push_back({"下一页", "textures/ui/arrow_right", ActionKind::NextPage, pageTarget(page_target, page + 1)});
    }
    form.buttons.push_back({"返回", "textures/ui/undoArrow", ActionKind::Back, ""});
    return form;
}

FormSpec ExchangeUiModel::productPage(const ProductView& view) const {
    FormSpec form;
    form.title = view.product.display_name;
    std::ostringstream body;
    body << "物品ID: " << view.product.item_id
         << "\n最高买价: " << money(view.quote.best_bid)
         << "\n最低卖价: " << money(view.quote.best_ask)
         << "\n差价: " << spread(view.quote)
         << "\n卖方库存: " << view.quote.ask_quantity
         << "\n买方需求: " << view.quote.bid_quantity
         << "\n最近成交: " << money(view.quote.last_price);
    form.body = body.str();
    form.buttons.push_back({"买入 - 市价", "textures/items/emerald", ActionKind::MarketBuy, view.product.product_key});
    form.buttons.push_back({"买入 - 挂单", "textures/items/paper", ActionKind::LimitBuy, view.product.product_key});
    form.buttons.push_back({"卖出 - 市价", "textures/items/chest", ActionKind::MarketSell, view.product.product_key});
    form.buttons.push_back({"卖出 - 挂单", "textures/items/writable_book", ActionKind::LimitSell, view.product.product_key});
    form.buttons.push_back({"订单簿", "textures/items/book_normal", ActionKind::OpenOrderBook, view.product.product_key});
    form.buttons.push_back({"返回", "textures/ui/undoArrow", ActionKind::Back, ""});
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
        form.buttons.push_back({"买入卖单 #" + std::to_string(ask.id), "textures/items/emerald", ActionKind::MarketBuy, std::to_string(ask.id)});
    }
    for (const auto& bid : book.bids) {
        form.buttons.push_back({"卖给买单 #" + std::to_string(bid.id), "textures/items/chest", ActionKind::MarketSell, std::to_string(bid.id)});
    }
    form.buttons.push_back({"返回", "textures/ui/undoArrow", ActionKind::Back, product.product_key});
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
            form.buttons.push_back({label.str(), "textures/ui/trash", ActionKind::CancelOrder, std::to_string(order.id)});
        } else {
            form.buttons.push_back({label.str(), "textures/items/paper", ActionKind::OpenProduct, order.product_key});
        }
    }
    form.buttons.push_back({"返回", "textures/ui/undoArrow", ActionKind::Back, ""});
    return form;
}

FormSpec ExchangeUiModel::mailboxPage(const std::vector<MailboxItem>& items) const {
    FormSpec form;
    form.title = "交易所邮箱";
    form.body = "成交和取消返还的物品会进入这里。";
    if (!items.empty()) {
        form.buttons.push_back({"全部领取", "textures/items/minecart_chest", ActionKind::ClaimAllMailbox, ""});
    }
    for (const auto& item : items) {
        std::ostringstream label;
        label << "#" << item.id << " x" << item.quantity << " " << item.product_key;
        form.buttons.push_back({label.str(), "textures/items/chest", ActionKind::ClaimMailboxItem, std::to_string(item.id)});
    }
    form.buttons.push_back({"返回", "textures/ui/undoArrow", ActionKind::Back, ""});
    return form;
}

FormSpec ExchangeUiModel::adminHome(const std::vector<Product>& stock_templates) const {
    FormSpec form;
    form.title = "交易所管理员";
    form.body = "为基建材料创建系统卖单，系统卖单成交收入只进入审计流水。";
    for (const auto& product : stock_templates) {
        form.buttons.push_back({"补货 " + product.display_name, product.icon, ActionKind::AdminRestock, product.product_key});
    }
    form.buttons.push_back({"返回", "textures/ui/undoArrow", ActionKind::Back, ""});
    return form;
}

}  // namespace exchange::ui
