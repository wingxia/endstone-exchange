#include "endstone_exchange/plugin/ExchangePlugin.hpp"

#include <unordered_map>

namespace exchange::plugin {

void ExchangePlugin::onEnable() {
    repository_ = std::make_unique<InMemoryRepository>();
    economy_ = std::make_unique<FakeEconomy>();
    service_ = std::make_unique<ExchangeService>(*repository_, *economy_);
    seedCatalog();
    getLogger().info("Endstone Exchange enabled");
}

void ExchangePlugin::onDisable() {
    getLogger().info("Endstone Exchange disabled");
}

bool ExchangePlugin::onCommand(endstone::CommandSender& sender, const endstone::Command& command, const std::vector<std::string>& args) {
    const auto name = command.getName();
    if (name != "ex" && name != "exchange") {
        return false;
    }
    auto* player = dynamic_cast<endstone::Player*>(&sender);
    if (player == nullptr) {
        sender.sendErrorMessage("Exchange UI can only be opened by players.");
        return true;
    }
    if (!args.empty() && args.front() == "admin") {
        openAdmin(*player);
        return true;
    }
    if (!args.empty() && args.front() == "reload") {
        seedCatalog();
        sender.sendMessage("Exchange catalog reloaded.");
        return true;
    }
    openHome(*player);
    return true;
}

void ExchangePlugin::seedCatalog() {
    const std::vector<std::tuple<std::string, std::string, std::string, std::string>> products = {
        {"minecraft:stone", "石头", "building", "textures/blocks/stone"},
        {"minecraft:dirt", "泥土", "building", "textures/blocks/dirt"},
        {"minecraft:oak_log", "橡木原木", "building", "textures/blocks/log_oak"},
        {"minecraft:iron_ingot", "铁锭", "ores", "textures/items/iron_ingot"},
        {"minecraft:diamond", "钻石", "ores", "textures/items/diamond"},
        {"minecraft:redstone", "红石粉", "redstone", "textures/items/redstone_dust"},
        {"minecraft:bread", "面包", "food", "textures/items/bread"},
        {"minecraft:diamond_pickaxe", "钻石镐", "tools", "textures/items/diamond_pickaxe"},
    };
    for (const auto& [item_id, name, category, icon] : products) {
        repository_->upsertProduct(Product{
            productKey(item_id, {}), item_id, {}, name, category, icon, true});
    }
}

void ExchangePlugin::openHome(endstone::Player& player) {
    const std::vector<ui::CategorySpec> categories = {
        {"building", "基建", "textures/blocks/stone"},
        {"ores", "矿物", "textures/items/diamond"},
        {"redstone", "红石", "textures/items/redstone_dust"},
        {"food", "食物", "textures/items/bread"},
        {"tools", "工具武器", "textures/items/diamond_pickaxe"},
        {"other", "其他", "textures/items/barrier"},
    };
    sendForm(player, ui_.home(categories, player.hasPermission("exchange.admin")));
}

void ExchangePlugin::openAdmin(endstone::Player& player) {
    sendForm(player, ui_.adminHome(service_->getCatalog("building")));
}

void ExchangePlugin::sendForm(endstone::Player& player, const ui::FormSpec& spec) {
    endstone::ActionForm form;
    form.setTitle(spec.title).setContent(spec.body);
    for (const auto& button : spec.buttons) {
        form.addButton(button.text, button.icon.empty() ? std::nullopt : std::optional<std::string>{button.icon});
    }
    player.sendForm(std::move(form));
}

PlayerRef ExchangePlugin::playerRef(endstone::Player& player) const {
    return PlayerRef{player.getUniqueId().str(), player.getName()};
}

}  // namespace exchange::plugin

ENDSTONE_PLUGIN("endstone_exchange", "0.1.0", exchange::plugin::ExchangePlugin) {
    prefix = "Exchange";
    description = "C++ item exchange with MySQL order book and UMoney bridge";
    website = "https://github.com/wingxia/endstone-exchange";
    authors = {"wingxia"};

    command("ex")
        .description("Open the exchange")
        .usages("/ex")
        .permissions("exchange.command.ex");
    command("exchange")
        .description("Open or manage the exchange")
        .usages("/exchange [admin|reload]")
        .permissions("exchange.command.ex");

    permission("exchange.command.ex")
        .description("Allow players to open the exchange")
        .default_(endstone::PermissionDefault::True);
    permission("exchange.admin")
        .description("Allow administrators to manage system stock")
        .default_(endstone::PermissionDefault::Operator);
}
