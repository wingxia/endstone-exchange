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

}  // namespace

ExchangeUiModel::ExchangeUiModel(std::size_t page_size) : page_size_(std::max<std::size_t>(6, page_size)) {}

FormSpec ExchangeUiModel::home(const std::vector<CategorySpec>& categories, bool admin) const {
    FormSpec form;
    form.title = "交易所";
    form.body = "选择分类、出售背包物品，或查看自己的订单。";
    form.buttons.push_back({"背包出售", "textures/items/chest", ActionKind::OpenInventorySell, ""});
    form.buttons.push_back({"一键全卖", "textures/items/emerald", ActionKind::SellAll, ""});
    form.buttons.push_back({"我的订单", "textures/items/paper", ActionKind::OpenMyOrders, ""});
    form.buttons.push_back({"邮箱领取", "textures/items/minecart_chest", ActionKind::OpenMailbox, ""});
    if (admin) {
        form.buttons.push_back({"管理员", "textures/items/gold_ingot", ActionKind::OpenAdmin, ""});
    }
    for (const auto& category : categories) {
        form.buttons.push_back({category.name, category.icon, ActionKind::OpenCategory, category.id});
    }
    return form;
}

FormSpec ExchangeUiModel::categoryPage(const CategorySpec& category, const std::vector<Product>& products, std::size_t page) const {
    FormSpec form;
    form.title = category.name;
    form.body = "选择商品。未配置商品会自动进入其他分类。";
    const auto start = page * page_size_;
    const auto end = std::min(products.size(), start + page_size_);
    if (page > 0) {
        form.buttons.push_back({"上一页", "textures/ui/arrow_left", ActionKind::PrevPage, category.id});
    }
    for (std::size_t i = start; i < end; ++i) {
        const auto& product = products[i];
        form.buttons.push_back({product.display_name, product.icon, ActionKind::OpenProduct, product.product_key});
    }
    if (end < products.size()) {
        form.buttons.push_back({"下一页", "textures/ui/arrow_right", ActionKind::NextPage, category.id});
    }
    form.buttons.push_back({"返回", "textures/ui/undoArrow", ActionKind::Back, ""});
    return form;
}

FormSpec ExchangeUiModel::productPage(const ProductView& view) const {
    FormSpec form;
    form.title = view.product.display_name;
    std::ostringstream body;
    body << "最高买价: " << money(view.quote.best_bid)
         << "\n最低卖价: " << money(view.quote.best_ask)
         << "\n差价: " << spread(view.quote)
         << "\n卖方库存: " << view.quote.ask_quantity
         << "\n买方需求: " << view.quote.bid_quantity
         << "\n最近成交: " << money(view.quote.last_price);
    form.body = body.str();
    form.buttons.push_back({"市价买", "textures/items/emerald", ActionKind::MarketBuy, view.product.product_key});
    form.buttons.push_back({"挂单买", "textures/items/paper", ActionKind::LimitBuy, view.product.product_key});
    form.buttons.push_back({"市价卖", "textures/items/chest", ActionKind::MarketSell, view.product.product_key});
    form.buttons.push_back({"挂单卖", "textures/items/writable_book", ActionKind::LimitSell, view.product.product_key});
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

