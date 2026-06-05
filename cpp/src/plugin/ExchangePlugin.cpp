#include "endstone_exchange/plugin/ExchangePlugin.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace exchange::plugin {
namespace {

struct BridgeConfig {
    std::string host{"127.0.0.1"};
    int port{8765};
    std::string token;
};

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::optional<BridgeConfig> loadBridgeConfig() {
    const auto path = std::filesystem::current_path() / "plugins" / "exchange_umoney_bridge" / "config.yml";
    std::ifstream file(path);
    if (!file) {
        return std::nullopt;
    }
    BridgeConfig config;
    std::string line;
    while (std::getline(file, line)) {
        const auto pos = line.find(':');
        if (pos == std::string::npos) {
            continue;
        }
        const auto key = trim(line.substr(0, pos));
        auto value = trim(line.substr(pos + 1));
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        if (key == "host") {
            config.host = value;
        } else if (key == "port") {
            config.port = std::stoi(value);
        } else if (key == "token") {
            config.token = value;
        }
    }
    if (config.token.empty()) {
        return std::nullopt;
    }
    return config;
}

bool looksLikeProductKey(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c); });
}

}  // namespace

void ExchangePlugin::onEnable() {
    repository_ = std::make_unique<InMemoryRepository>();
    if (auto bridge = loadBridgeConfig()) {
        economy_ = std::make_unique<HttpBridgeEconomy>(bridge->host, bridge->port, bridge->token);
        getLogger().info("Exchange economy connected to UMoney bridge at " + bridge->host + ":" + std::to_string(bridge->port));
    } else {
        economy_ = std::make_unique<FakeEconomy>();
        getLogger().warning("Exchange UMoney bridge config was not found; using in-memory test economy");
    }
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
    if (!args.empty() && args.front() == "reload") {
        seedCatalog();
        sender.sendMessage("Exchange catalog reloaded.");
        return true;
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
    sendForm(player, ui_.home(categories(), player.hasPermission("exchange.admin")));
}

void ExchangePlugin::openCategory(endstone::Player& player, const std::string& category_id, std::size_t page) {
    auto all_categories = categories();
    auto it = std::find_if(all_categories.begin(), all_categories.end(), [&](const ui::CategorySpec& category) {
        return category.id == category_id;
    });
    if (it == all_categories.end()) {
        sendNotice(player, "分类不存在。");
        openHome(player);
        return;
    }
    sendForm(player, ui_.categoryPage(*it, service_->getCatalog(category_id), page));
}

void ExchangePlugin::openProduct(endstone::Player& player, const std::string& product_key) {
    auto product = repository_->getProduct(product_key);
    if (!product) {
        sendNotice(player, "商品不存在。");
        openHome(player);
        return;
    }
    sendForm(player, ui_.productPage(ui::ProductView{*product, service_->getQuote(product_key)}));
}

void ExchangePlugin::openOrderBook(endstone::Player& player, const std::string& product_key) {
    auto product = repository_->getProduct(product_key);
    if (!product) {
        sendNotice(player, "商品不存在。");
        openHome(player);
        return;
    }
    sendForm(player, ui_.orderBookPage(*product, service_->listOrderBook(product_key, 12)));
}

void ExchangePlugin::openAdmin(endstone::Player& player) {
    sendForm(player, ui_.adminHome(service_->getCatalog("building")));
}

void ExchangePlugin::sendForm(endstone::Player& player, const ui::FormSpec& spec) {
    endstone::ActionForm form;
    form.setTitle(spec.title).setContent(spec.body);
    for (const auto& button : spec.buttons) {
        form.addButton(button.text, button.icon.empty() ? std::nullopt : std::optional<std::string>{button.icon},
                       [this, button](endstone::Player* clicked) {
                           if (clicked != nullptr) {
                               handleAction(*clicked, button);
                           }
                       });
    }
    player.sendForm(std::move(form));
}

void ExchangePlugin::handleAction(endstone::Player& player, const ui::ButtonSpec& button) {
    try {
        switch (button.action) {
        case ui::ActionKind::OpenCategory:
            openCategory(player, button.target);
            return;
        case ui::ActionKind::OpenProduct:
            openProduct(player, button.target);
            return;
        case ui::ActionKind::OpenOrderBook:
            openOrderBook(player, button.target);
            return;
        case ui::ActionKind::OpenAdmin:
            if (!player.hasPermission("exchange.admin")) {
                sendNotice(player, "没有管理员权限。");
                return;
            }
            openAdmin(player);
            return;
        case ui::ActionKind::AdminRestock: {
            if (!player.hasPermission("exchange.admin")) {
                sendNotice(player, "没有管理员权限。");
                return;
            }
            auto order = service_->adminCreateSystemSellOrder(playerRef(player), button.target, 64, 1);
            sendNotice(player, "已补货系统卖单 #" + std::to_string(order.id) + "，数量 64，单价 1。");
            openProduct(player, button.target);
            return;
        }
        case ui::ActionKind::MarketBuy:
            if (!looksLikeProductKey(button.target)) {
                sendNotice(player, "指定订单成交仍在开发中，请从商品页使用市价买。");
                return;
            }
            service_->placeMarketBuy(playerRef(player), button.target, 1);
            sendNotice(player, "市价买入 1 个，成交物品已进入交易所邮箱。");
            openProduct(player, button.target);
            return;
        case ui::ActionKind::LimitBuy:
            service_->placeLimitBuy(playerRef(player), button.target, 1, 1);
            sendNotice(player, "已挂买单：数量 1，单价 1。");
            openProduct(player, button.target);
            return;
        case ui::ActionKind::MarketSell:
        case ui::ActionKind::LimitSell:
        case ui::ActionKind::OpenInventorySell:
        case ui::ActionKind::SellAll:
            sendNotice(player, "背包扣除、NBT 托管和一键出售正在接入，当前测试服先验证买入/订单簿/管理员补货。");
            return;
        case ui::ActionKind::OpenMyOrders: {
            auto orders = service_->listPlayerOrders(playerRef(player), true);
            sendNotice(player, "我的订单数量: " + std::to_string(orders.size()));
            return;
        }
        case ui::ActionKind::OpenMailbox:
            sendNotice(player, "邮箱 UI 正在接入；当前成交会写入后端 mailbox。");
            return;
        case ui::ActionKind::Back:
            if (looksLikeProductKey(button.target)) {
                openProduct(player, button.target);
            } else {
                openHome(player);
            }
            return;
        case ui::ActionKind::NextPage:
        case ui::ActionKind::PrevPage:
            openCategory(player, button.target);
            return;
        }
    } catch (const ExchangeException& exc) {
        sendNotice(player, std::string("交易失败: ") + exc.what());
    } catch (const std::exception& exc) {
        sendNotice(player, std::string("交易所错误: ") + exc.what());
    }
}

void ExchangePlugin::sendNotice(endstone::Player& player, const std::string& message) {
    player.sendMessage("[交易所] " + message);
}

std::vector<ui::CategorySpec> ExchangePlugin::categories() const {
    return {
        {"building", "基建", "textures/blocks/stone"},
        {"ores", "矿物", "textures/items/diamond"},
        {"redstone", "红石", "textures/items/redstone_dust"},
        {"food", "食物", "textures/items/bread"},
        {"tools", "工具武器", "textures/items/diamond_pickaxe"},
        {"other", "其他", "textures/items/barrier"},
    };
}

PlayerRef ExchangePlugin::playerRef(endstone::Player& player) const {
    return PlayerRef{player.getUniqueId().str(), player.getName()};
}

}  // namespace exchange::plugin

ENDSTONE_PLUGIN("exchange", "0.1.0", exchange::plugin::ExchangePlugin) {
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
