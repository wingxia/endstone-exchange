#include <cassert>
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
    return Product{key, normalizeIdentifier(item_id), enchants, item_id, "test", "icon", true};
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

void test_ui_model_contains_expected_actions() {
    ui::ExchangeUiModel model(8);
    const auto form = model.home({{"building", "基建", "textures/blocks/stone"}}, true);
    assert(form.buttons.size() >= 6);
    assert(form.buttons[0].action == ui::ActionKind::OpenSearch);
    assert(form.buttons[1].action == ui::ActionKind::OpenAllProducts);
}

void test_ui_model_search_results_page_targets() {
    ui::ExchangeUiModel model(6);
    const auto diamond = productFor("minecraft:diamond");
    const auto stone = productFor("minecraft:stone");
    const auto bread = productFor("minecraft:bread");
    const auto apple = productFor("minecraft:apple");
    const auto coal = productFor("minecraft:coal");
    const auto emerald = productFor("minecraft:emerald");
    const auto iron = productFor("minecraft:iron_ingot");
    const auto form = model.searchResults("stone", {diamond, stone, bread, apple, coal, emerald, iron}, 0);
    assert(form.buttons.size() == 8);
    assert(form.buttons[0].action == ui::ActionKind::OpenProduct);
    assert(form.buttons[6].action == ui::ActionKind::NextPage);
    assert(form.buttons[6].target == "search|1");
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
    test_mailbox_claim_marks_item();
    test_ui_model_contains_expected_actions();
    test_ui_model_search_results_page_targets();
    std::cout << "exchange_core_tests passed\n";
    return 0;
}
