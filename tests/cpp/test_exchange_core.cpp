#include <cassert>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

#include "endstone_exchange/core/Economy.hpp"
#include "endstone_exchange/core/ExchangeService.hpp"
#include "endstone_exchange/core/ItemIdentity.hpp"
#include "endstone_exchange/core/Repository.hpp"
#include "endstone_exchange/catalog/GeneratedCatalog.hpp"
#include "endstone_exchange/ui/ExchangeUiModel.hpp"

using namespace exchange;

namespace {

Product productFor(const std::string& item_id, std::vector<Enchantment> enchants = {}) {
    const auto key = productKey(item_id, enchants);
    return Product{key, normalizeIdentifier(item_id), enchants, item_id, "common", "textures/items/paper", true};
}

ItemSnapshot snapshotFor(const Product& product, std::int32_t quantity) {
    return ItemSnapshot{product.product_key, product.item_id, product.enchants, quantity, {1, 2, 3}, "nbt"};
}

void test_product_key_ignores_non_enchant_nbt() {
    const auto first = productKey("minecraft:diamond_sword", {{"sharpness", 5}});
    const auto second = productKey("minecraft:diamond_sword", {{"minecraft:sharpness", 5}});
    const auto plain = productKey("minecraft:diamond_sword", {});
    assert(first == second);
    assert(first != plain);
    assert(productKey("diamond_sword", {{"sharpness", 5}}) == first);
}

void test_limit_buy_freezes_and_limit_sell_matches_with_refund() {
    InMemoryRepository repo;
    FakeEconomy economy({{"Buyer", 1000}, {"Seller", 0}});
    ExchangeService service(repo, economy);
    const auto diamond = productFor("minecraft:diamond");
    repo.upsertProduct(diamond);

    const PlayerRef buyer{"buyer-uuid", "Buyer"};
    const PlayerRef seller{"seller-uuid", "Seller"};
    const auto buy = service.placeLimitBuy(buyer, diamond.product_key, 10, 50);
    assert(buy.remaining_quantity == 10);
    assert(economy.balance("Buyer") == 500);

    const auto sell = service.placeLimitSell(seller, diamond.product_key, {snapshotFor(diamond, 4)}, 40);
    assert(sell.status == OrderStatus::Filled);
    const auto updated_buy = repo.getOrder(buy.id).value();
    assert(updated_buy.remaining_quantity == 6);
    assert(updated_buy.status == OrderStatus::Partial);

    service.processSettlements();
    assert(economy.balance("Seller") == 200);
    assert(economy.balance("Buyer") == 500);
    assert(repo.mailboxForPlayer("buyer-uuid").size() == 1);
    assert(repo.mailboxForPlayer("buyer-uuid").front().quantity == 4);
}

void test_limit_buy_gets_price_improvement_against_existing_ask() {
    InMemoryRepository repo;
    FakeEconomy economy({{"Buyer", 1000}, {"Seller", 0}});
    ExchangeService service(repo, economy);
    const auto diamond = productFor("minecraft:diamond");
    repo.upsertProduct(diamond);

    const PlayerRef buyer{"buyer-uuid", "Buyer"};
    const PlayerRef seller{"seller-uuid", "Seller"};
    service.placeLimitSell(seller, diamond.product_key, {snapshotFor(diamond, 2)}, 40);
    const auto buy = service.placeLimitBuy(buyer, diamond.product_key, 2, 50);
    assert(buy.status == OrderStatus::Filled);
    assert(economy.balance("Seller") == 80);
    assert(economy.balance("Buyer") == 920);
}

void test_market_sell_only_sells_to_existing_bids() {
    InMemoryRepository repo;
    FakeEconomy economy({{"Buyer", 1000}, {"Seller", 0}});
    ExchangeService service(repo, economy);
    const auto stone = productFor("minecraft:stone");
    repo.upsertProduct(stone);
    const PlayerRef buyer{"buyer-uuid", "Buyer"};
    const PlayerRef seller{"seller-uuid", "Seller"};

    service.placeLimitBuy(buyer, stone.product_key, 5, 8);
    const auto sell = service.placeMarketSell(seller, stone.product_key, {snapshotFor(stone, 20)});
    assert(sell.original_quantity == 5);
    assert(sell.status == OrderStatus::Filled);
    assert(economy.balance("Seller") == 40);

    bool failed = false;
    try {
        service.placeMarketSell(seller, stone.product_key, {snapshotFor(stone, 1)});
    } catch (const InvalidOrder&) {
        failed = true;
    }
    assert(failed);
}

void test_cancel_refunds_buy_order() {
    InMemoryRepository repo;
    FakeEconomy economy({{"Buyer", 500}});
    ExchangeService service(repo, economy);
    const auto bread = productFor("minecraft:bread");
    repo.upsertProduct(bread);
    const PlayerRef buyer{"buyer-uuid", "Buyer"};

    const auto order = service.placeLimitBuy(buyer, bread.product_key, 10, 10);
    assert(economy.balance("Buyer") == 400);
    const auto cancelled = service.cancelOrder(buyer, order.id);
    assert(cancelled.status == OrderStatus::Cancelled);
    assert(economy.balance("Buyer") == 500);
}

void test_admin_system_stock() {
    InMemoryRepository repo;
    FakeEconomy economy({{"Builder", 100}});
    ExchangeService service(repo, economy);
    const auto stone = productFor("minecraft:stone");
    repo.upsertProduct(stone);
    const PlayerRef admin{"admin-uuid", "Admin"};
    const PlayerRef builder{"builder-uuid", "Builder"};

    auto system = service.adminCreateSystemSellOrder(admin, stone.product_key, 64, 2);
    assert(system.system_order);
    const auto bought = service.placeMarketBuy(builder, stone.product_key, 10);
    assert(bought.status == OrderStatus::Filled);
    assert(economy.balance("Builder") == 80);
    assert(repo.mailboxForPlayer("builder-uuid").front().quantity == 10);
    assert(economy.balance("ExchangeSystem") == 0);
}

void test_generated_catalog_is_searchable() {
    assert(exchange::catalog::products().size() > 700);
    InMemoryRepository repo;
    FakeEconomy economy;
    ExchangeService service(repo, economy);
    for (const auto& product : exchange::catalog::products()) {
        repo.upsertProduct(product);
    }

    const auto diamond = service.searchCatalog("diamond");
    assert(!diamond.empty());
    assert(std::any_of(diamond.begin(), diamond.end(), [](const Product& product) {
        return product.item_id == "minecraft:diamond";
    }));

    const auto oak = service.searchCatalog("oak_log");
    assert(!oak.empty());
    assert(std::any_of(oak.begin(), oak.end(), [](const Product& product) {
        return product.item_id == "minecraft:oak_log";
    }));
}

void test_generated_catalog_has_complete_categories_and_icons() {
    assert(exchange::catalog::products().size() >= 1866);
    assert(!exchange::catalog::categories().empty());

    std::unordered_map<std::string, bool> categories;
    for (const auto& category : exchange::catalog::categories()) {
        assert(!category.id.empty());
        assert(!category.name.empty());
        assert(!category.icon.empty());
        categories.emplace(category.id, false);
    }

    for (const auto& product : exchange::catalog::products()) {
        assert(!product.product_key.empty());
        assert(!product.item_id.empty());
        assert(product.item_id.rfind("minecraft:", 0) == 0);
        assert(!product.display_name.empty());
        assert(!product.category.empty());
        assert(categories.count(product.category) == 1);
        assert(!product.icon.empty());
        assert(product.icon.rfind("textures/items/", 0) == 0 || product.icon.rfind("textures/blocks/", 0) == 0);
        categories[product.category] = true;
    }

    for (const auto& [category, has_product] : categories) {
        assert(has_product);
    }
}

void test_mailbox_claim_marks_item() {
    InMemoryRepository repo;
    FakeEconomy economy;
    ExchangeService service(repo, economy);
    const PlayerRef buyer{"buyer-uuid", "Buyer"};

    repo.addMailboxItem(MailboxItem{0, buyer.uuid, buyer.name, productKey("minecraft:diamond", {}), 3, {}, "diamond", false});
    auto mailbox = service.listMailbox(buyer);
    assert(mailbox.size() == 1);

    const auto claimed = service.markMailboxClaimed(buyer, mailbox.front().id);
    assert(claimed.claimed);
    assert(service.listMailbox(buyer).empty());

    bool failed = false;
    try {
        service.markMailboxClaimed(buyer, mailbox.front().id);
    } catch (const InvalidOrder&) {
        failed = true;
    }
    assert(failed);
}

ui::DashboardView sampleDashboardView() {
    const std::vector<ui::CategorySpec> categories{
        {"building", "建筑方块", "textures/blocks/stone"},
        {"ores", "矿物材料", "textures/items/diamond"},
    };
    auto stone = productFor("minecraft:stone");
    stone.category = "building";
    stone.icon = "textures/blocks/stone";
    auto diamond = productFor("minecraft:diamond");
    diamond.category = "building";
    diamond.icon = "textures/items/diamond";
    return ui::DashboardView{
        categories,
        "building",
        "建筑方块",
        0,
        1,
        {
            ui::DashboardProductView{stone, Quote{stone.product_key, 3, 5, 12, 8, 4}, false},
            ui::DashboardProductView{diamond, Quote{diamond.product_key, 10, 12, 5, 7, 11}, true},
        },
        1,
        3,
        28,
        ui::ProductView{diamond, Quote{diamond.product_key, 10, 12, 5, 7, 11}},
        1000,
        true,
        ""};
}

void test_ui_model_dashboard_sections_and_trade_actions() {
    ui::ExchangeUiModel model(6);
    const auto view = sampleDashboardView();
    const auto form = model.dashboard(view);

    assert(form.title == "Exchange Chest UI");
    assert(form.body.find("箱子式交易面板") != std::string::npos);
    assert(form.body.find("余额: 1000") != std::string::npos);
    assert(form.body.find("分类: 建筑方块") != std::string::npos);
    assert(form.body.find("分类组: 1 / 1") != std::string::npos);
    assert(form.body.find("最高买价: 10") != std::string::npos);
    assert(form.body.find("最低卖价: 12") != std::string::npos);
    assert(form.body.find("最近成交: 11") != std::string::npos);
    assert(form.buttons.size() == ui::dashboard_layout::kTotalButtons);
    assert(form.controls.size() == ui::dashboard_layout::kTotalButtons);

    const auto trade_start = ui::dashboard_layout::kTradeStart;
    assert(form.buttons[trade_start].action == ui::ActionKind::MarketBuy);
    assert(form.buttons[trade_start + 1].action == ui::ActionKind::LimitBuy);
    assert(form.buttons[trade_start + 2].action == ui::ActionKind::MarketSell);
    assert(form.buttons[trade_start + 3].action == ui::ActionKind::LimitSell);
    assert(form.buttons[trade_start + 4].action == ui::ActionKind::OpenOrderBook);
    for (std::size_t i = trade_start; i < trade_start + ui::dashboard_layout::kTradeSlots; ++i) {
        assert(form.buttons[i].target == view.selected_product->product.product_key);
        assert(!form.buttons[i].icon.empty());
    }
}

void test_ui_model_dashboard_category_and_product_controls() {
    ui::ExchangeUiModel model(6);
    const auto view = sampleDashboardView();
    const auto form = model.dashboard(view);

    const auto category_start = ui::dashboard_layout::kCategoryStart;
    assert(form.buttons[category_start].action == ui::ActionKind::DashboardCategory);
    assert(form.buttons[category_start].target == "building");
    assert(form.buttons[category_start].text.find("当前") != std::string::npos);
    assert(form.buttons[category_start].icon == "textures/blocks/stone");
    assert(form.buttons[category_start + 1].action == ui::ActionKind::DashboardCategory);
    assert(form.buttons[category_start + 1].target == "ores");
    assert(form.buttons[category_start + 1].icon == "textures/items/diamond");
    assert(form.buttons[category_start + 2].action == ui::ActionKind::Noop);

    const auto product_start = ui::dashboard_layout::kProductStart;
    assert(form.buttons[product_start].action == ui::ActionKind::DashboardProduct);
    assert(form.buttons[product_start].text.find('\n') == std::string::npos);
    assert(form.buttons[product_start].text.find("minecraft:stone") == std::string::npos);
    assert(form.buttons[product_start].text.find("买 3 / 卖 5") == std::string::npos);
    assert(form.buttons[product_start].icon == "textures/blocks/stone");
    assert(form.buttons[product_start + 1].action == ui::ActionKind::DashboardProduct);
    assert(form.buttons[product_start + 1].text.find("选 ") != std::string::npos);
    assert(form.buttons[product_start + 1].text.find("minecraft:diamond") == std::string::npos);
    assert(form.buttons[product_start + 1].target == view.selected_product->product.product_key);
    assert(form.buttons[product_start + 1].icon == "textures/items/diamond");
    assert(form.buttons[product_start + 2].action == ui::ActionKind::Noop);
}

void test_ui_model_dashboard_pagination_and_tools() {
    ui::ExchangeUiModel model(6);
    const auto form = model.dashboard(sampleDashboardView());

    const auto tool_start = ui::dashboard_layout::kToolStart;
    assert(form.buttons[tool_start].action == ui::ActionKind::DashboardPage);
    assert(form.buttons[tool_start].text == "上页");
    assert(form.buttons[tool_start].target == "0");
    assert(form.buttons[tool_start + 1].action == ui::ActionKind::DashboardPage);
    assert(form.buttons[tool_start + 1].text == "下页");
    assert(form.buttons[tool_start + 1].target == "2");
    assert(form.buttons[tool_start + 2].action == ui::ActionKind::OpenSearch);
    assert(form.buttons[tool_start + 2].text == "搜索");
    assert(form.buttons[tool_start + 3].action == ui::ActionKind::OpenAllProducts);
    assert(form.buttons[tool_start + 3].text == "全部");
    assert(form.buttons[tool_start + 4].action == ui::ActionKind::OpenMyOrders);
    assert(form.buttons[tool_start + 4].text == "订单");
    assert(form.buttons[tool_start + 5].action == ui::ActionKind::OpenMailbox);
    assert(form.buttons[tool_start + 5].text == "邮箱");
    assert(form.buttons[tool_start + 6].action == ui::ActionKind::OpenAdmin);
    assert(form.buttons[tool_start + 6].text == "管理");

    const auto trade_start = ui::dashboard_layout::kTradeStart;
    assert(form.buttons[trade_start].text == "买市");
    assert(form.buttons[trade_start + 1].text == "买挂");
    assert(form.buttons[trade_start + 2].text == "卖市");
    assert(form.buttons[trade_start + 3].text == "卖挂");
    assert(form.buttons[trade_start + 4].text == "簿");
}

void test_ui_model_dashboard_category_group_slot() {
    ui::ExchangeUiModel model(6);
    auto view = sampleDashboardView();
    view.categories.clear();
    for (std::size_t i = 0; i < ui::dashboard_layout::kCategorySlots - 1; ++i) {
        view.categories.push_back({"cat" + std::to_string(i), "分类" + std::to_string(i), "textures/items/paper"});
    }
    view.category_page = 0;
    view.total_category_pages = 2;

    const auto form = model.dashboard(view);
    const auto nav_index = ui::dashboard_layout::kCategoryStart + ui::dashboard_layout::kCategorySlots - 1;
    assert(form.buttons[nav_index].action == ui::ActionKind::DashboardCategoryPage);
    assert(form.buttons[nav_index].target == "1");
    assert(form.buttons[nav_index].text.find("更多") != std::string::npos);
}

void test_ui_model_subpages_use_fixed_frame() {
    ui::ExchangeUiModel model(6);
    auto diamond = productFor("minecraft:diamond");
    diamond.icon = "textures/items/diamond";
    const auto product_page = model.productPage(ui::ProductView{diamond, Quote{diamond.product_key, 1, 2, 3, 4, 5}});
    assert(product_page.buttons.size() == ui::dashboard_layout::kTotalButtons);
    assert(product_page.buttons[ui::dashboard_layout::kTradeStart].text == "买市");
    assert(product_page.buttons[ui::dashboard_layout::kTradeStart + 4].text == "返");

    const auto mailbox = model.mailboxPage({MailboxItem{1, "uuid", "name", diamond.product_key, 3, {}, "diamond", false}});
    assert(mailbox.buttons.size() == ui::dashboard_layout::kTotalButtons);
    assert(mailbox.buttons[ui::dashboard_layout::kProductStart].text == "全领");
    assert(mailbox.buttons[ui::dashboard_layout::kTradeStart + 4].action == ui::ActionKind::Back);
}

}  // namespace

int main() {
    test_product_key_ignores_non_enchant_nbt();
    test_limit_buy_freezes_and_limit_sell_matches_with_refund();
    test_limit_buy_gets_price_improvement_against_existing_ask();
    test_market_sell_only_sells_to_existing_bids();
    test_cancel_refunds_buy_order();
    test_admin_system_stock();
    test_generated_catalog_is_searchable();
    test_generated_catalog_has_complete_categories_and_icons();
    test_mailbox_claim_marks_item();
    test_ui_model_dashboard_sections_and_trade_actions();
    test_ui_model_dashboard_category_and_product_controls();
    test_ui_model_dashboard_pagination_and_tools();
    test_ui_model_dashboard_category_group_slot();
    test_ui_model_subpages_use_fixed_frame();
    std::cout << "exchange_core_tests passed\n";
    return 0;
}
