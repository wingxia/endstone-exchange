#include "endstone_exchange/plugin/ExchangePlugin.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <climits>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_map>

#include <endstone/color_format.h>
#include <endstone/form/action_form.h>
#include <endstone/form/controls/text_input.h>
#include <endstone/form/modal_form.h>
#include <endstone/inventory/item_stack.h>

#include "endstone_exchange/catalog/GeneratedCatalog.hpp"
namespace exchange::plugin {
namespace {

constexpr std::size_t kDashboardPageSize = ui::dashboard_layout::kProductSlots;

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

std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : value) {
        if (c == delimiter) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    return parts;
}

std::vector<std::string> parseModalValues(const std::string& json) {
    std::vector<std::string> out;
    std::size_t i = 0;
    auto skip = [&]() {
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) {
            ++i;
        }
    };
    skip();
    if (i >= json.size() || json[i] != '[') {
        return out;
    }
    ++i;
    while (i < json.size()) {
        skip();
        if (i >= json.size() || json[i] == ']') {
            break;
        }
        if (json[i] == '"') {
            ++i;
            std::string value;
            while (i < json.size()) {
                const char c = json[i++];
                if (c == '"') {
                    break;
                }
                if (c == '\\' && i < json.size()) {
                    value.push_back(json[i++]);
                } else {
                    value.push_back(c);
                }
            }
            out.push_back(value);
        } else {
            const auto start = i;
            while (i < json.size() && json[i] != ',' && json[i] != ']') {
                ++i;
            }
            out.push_back(trim(json.substr(start, i - start)));
        }
        skip();
        if (i < json.size() && json[i] == ',') {
            ++i;
        }
    }
    return out;
}

std::optional<std::int32_t> parsePositiveInt32(const std::string& value) {
    std::int64_t parsed = 0;
    const auto trimmed = trim(value);
    const auto* first = trimmed.data();
    const auto* last = trimmed.data() + trimmed.size();
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last || parsed <= 0 || parsed > INT32_MAX) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(parsed);
}

std::optional<std::int64_t> parsePositiveInt64(const std::string& value) {
    std::int64_t parsed = 0;
    const auto trimmed = trim(value);
    const auto* first = trimmed.data();
    const auto* last = trimmed.data() + trimmed.size();
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last || parsed <= 0) {
        return std::nullopt;
    }
    return parsed;
}

std::string actionToken(ui::ActionKind action) {
    switch (action) {
    case ui::ActionKind::MarketBuy:
        return "market_buy";
    case ui::ActionKind::LimitBuy:
        return "limit_buy";
    case ui::ActionKind::MarketSell:
        return "market_sell";
    case ui::ActionKind::LimitSell:
        return "limit_sell";
    default:
        return "unknown";
    }
}

std::optional<ui::ActionKind> actionFromToken(const std::string& token) {
    if (token == "market_buy") {
        return ui::ActionKind::MarketBuy;
    }
    if (token == "limit_buy") {
        return ui::ActionKind::LimitBuy;
    }
    if (token == "market_sell") {
        return ui::ActionKind::MarketSell;
    }
    if (token == "limit_sell") {
        return ui::ActionKind::LimitSell;
    }
    return std::nullopt;
}

std::string tradePayload(ui::ActionKind action, const std::string& product_key, std::int32_t quantity, std::int64_t unit_price) {
    return actionToken(action) + "|" + product_key + "|" + std::to_string(quantity) + "|" + std::to_string(unit_price);
}

std::pair<std::string, std::size_t> parsePageTarget(const std::string& target) {
    const auto parts = split(target, '|');
    if (parts.size() < 2) {
        return {target, 0};
    }
    auto page = parsePositiveInt64(parts[1]);
    return {parts[0], page ? static_cast<std::size_t>(*page) : 0};
}

std::optional<std::size_t> parsePageIndex(const std::string& value) {
    std::int64_t parsed = 0;
    const auto trimmed = trim(value);
    const auto* first = trimmed.data();
    const auto* last = trimmed.data() + trimmed.size();
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last || parsed < 0) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(parsed);
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
    auto* player = sender.asPlayer();
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
    for (const auto& product : catalog::products()) {
        repository_->upsertProduct(product);
    }
    getLogger().info("Exchange catalog loaded " + std::to_string(catalog::products().size()) + " products from generated Minecraft Wiki data");
}

void ExchangePlugin::openHome(endstone::Player& player) {
    openDashboard(player);
}

void ExchangePlugin::openDashboard(endstone::Player& player) {
    std::optional<std::int64_t> balance;
    try {
        balance = economy_->balance(player.getName());
    } catch (...) {
        balance = std::nullopt;
    }

    auto all_categories = categories();
    auto& state = dashboardState(player);
    if (all_categories.empty()) {
        sendForm(player, ui_.dashboard(ui::DashboardView{{}, "", "", 0, 1, {}, 0, 1, 0, std::nullopt, balance, player.hasPermission("exchange.admin"), state.search_query}));
        return;
    }

    const auto category_exists = [&](const std::string& category_id) {
        return std::any_of(all_categories.begin(), all_categories.end(), [&](const ui::CategorySpec& category) {
            return category.id == category_id;
        });
    };
    if (state.search_query.empty() && !state.category_id.empty() && !category_exists(state.category_id)) {
        state.category_id.clear();
        state.category_page = 0;
        state.page = 0;
        state.product_key.clear();
    }

    const auto category_page_size = all_categories.size() > ui::dashboard_layout::kCategorySlots
                                        ? ui::dashboard_layout::kCategorySlots - 1
                                        : ui::dashboard_layout::kCategorySlots;
    const auto total_category_pages = std::max<std::size_t>(1, (all_categories.size() + category_page_size - 1) / category_page_size);
    if (state.category_page >= total_category_pages) {
        state.category_page = total_category_pages - 1;
    }
    const auto active_category_it = std::find_if(all_categories.begin(), all_categories.end(), [&](const ui::CategorySpec& category) {
        return category.id == state.category_id;
    });
    if (active_category_it != all_categories.end()) {
        const auto active_index = static_cast<std::size_t>(std::distance(all_categories.begin(), active_category_it));
        const auto active_page = active_index / category_page_size;
        if (active_page != state.category_page) {
            state.category_page = active_page;
        }
    }
    const auto category_start = std::min(all_categories.size(), state.category_page * category_page_size);
    const auto category_end = std::min(all_categories.size(), category_start + category_page_size);
    std::vector<ui::CategorySpec> visible_categories;
    visible_categories.reserve(category_end - category_start);
    for (std::size_t i = category_start; i < category_end; ++i) {
        visible_categories.push_back(all_categories[i]);
    }

    std::vector<Product> products;
    std::string active_category_name;
    if (!state.search_query.empty()) {
        products = service_->searchCatalog(state.search_query);
        active_category_name = "搜索结果";
    } else if (state.category_id.empty()) {
        products = service_->getCatalog(std::nullopt);
        active_category_name = "全部物品";
    } else {
        products = service_->getCatalog(state.category_id);
        auto category_it = std::find_if(all_categories.begin(), all_categories.end(), [&](const ui::CategorySpec& category) {
            return category.id == state.category_id;
        });
        active_category_name = category_it == all_categories.end() ? state.category_id : category_it->name;
    }

    const auto total_pages = std::max<std::size_t>(1, (products.size() + kDashboardPageSize - 1) / kDashboardPageSize);
    if (state.page >= total_pages) {
        state.page = total_pages - 1;
    }
    const auto start = std::min(products.size(), state.page * kDashboardPageSize);
    const auto end = std::min(products.size(), start + kDashboardPageSize);

    const auto selected_on_page = std::any_of(products.begin() + static_cast<std::ptrdiff_t>(start), products.begin() + static_cast<std::ptrdiff_t>(end), [&](const Product& product) {
        return product.product_key == state.product_key;
    });
    if (!selected_on_page) {
        state.product_key = start < end ? products[start].product_key : "";
    }

    std::vector<ui::DashboardProductView> product_views;
    product_views.reserve(end - start);
    for (std::size_t i = start; i < end; ++i) {
        product_views.push_back({products[i], service_->getQuote(products[i].product_key), products[i].product_key == state.product_key});
    }

    std::optional<ui::ProductView> selected;
    if (!state.product_key.empty()) {
        auto product = repository_->getProduct(state.product_key);
        if (product) {
            selected = ui::ProductView{*product, service_->getQuote(product->product_key)};
        }
    }

    sendForm(player, ui_.dashboard(ui::DashboardView{
                         visible_categories,
                         state.category_id,
                         active_category_name,
                         state.category_page,
                         total_category_pages,
                         product_views,
                         state.page,
                         total_pages,
                         products.size(),
                         selected,
                         balance,
                         player.hasPermission("exchange.admin"),
                         state.search_query}));
}

void ExchangePlugin::openDashboardCategory(endstone::Player& player, const std::string& category_id) {
    auto all_categories = categories();
    const auto exists = std::any_of(all_categories.begin(), all_categories.end(), [&](const ui::CategorySpec& category) {
        return category.id == category_id;
    });
    if (!exists) {
        sendNotice(player, "分类不存在。");
        openDashboard(player);
        return;
    }
    auto& state = dashboardState(player);
    state.category_id = category_id;
    state.search_query.clear();
    state.category_page = 0;
    state.page = 0;
    state.product_key.clear();
    search_queries_.erase(player.getUniqueId().str());
    openDashboard(player);
}

void ExchangePlugin::openDashboardCategoryPage(endstone::Player& player, std::size_t page) {
    auto& state = dashboardState(player);
    state.category_page = page;
    openDashboard(player);
}

void ExchangePlugin::openDashboardProduct(endstone::Player& player, const std::string& product_key) {
    auto product = repository_->getProduct(product_key);
    if (!product) {
        sendNotice(player, "商品不存在。");
        openDashboard(player);
        return;
    }

    auto& state = dashboardState(player);
    std::vector<Product> products;
    if (!state.search_query.empty()) {
        products = service_->searchCatalog(state.search_query);
    }
    auto it = std::find_if(products.begin(), products.end(), [&](const Product& candidate) {
        return candidate.product_key == product_key;
    });
    if (it == products.end()) {
        state.search_query.clear();
        search_queries_.erase(player.getUniqueId().str());
        state.category_id = product->category;
        state.category_page = 0;
        products = service_->getCatalog(state.category_id);
        it = std::find_if(products.begin(), products.end(), [&](const Product& candidate) {
            return candidate.product_key == product_key;
        });
    }
    state.product_key = product_key;
    if (it != products.end()) {
        const auto index = static_cast<std::size_t>(std::distance(products.begin(), it));
        state.page = index / kDashboardPageSize;
    }
    openDashboard(player);
}

void ExchangePlugin::openDashboardPage(endstone::Player& player, std::size_t page) {
    auto& state = dashboardState(player);
    state.page = page;
    state.product_key.clear();
    openDashboard(player);
}

void ExchangePlugin::resetDashboard(endstone::Player& player) {
    auto& state = dashboardState(player);
    state.category_id.clear();
    state.category_page = 0;
    state.page = 0;
    state.product_key.clear();
    state.search_query.clear();
    search_queries_.erase(player.getUniqueId().str());
    openDashboard(player);
}

void ExchangePlugin::openCategory(endstone::Player& player, const std::string& category_id, std::size_t page) {
    const auto all_categories = categories();
    const auto exists = std::any_of(all_categories.begin(), all_categories.end(), [&](const ui::CategorySpec& category) {
        return category.id == category_id;
    });
    if (!exists) {
        sendNotice(player, "分类不存在。");
        openDashboard(player);
        return;
    }
    auto& state = dashboardState(player);
    state.category_id = category_id;
    state.search_query.clear();
    state.category_page = 0;
    state.page = page;
    state.product_key.clear();
    search_queries_.erase(player.getUniqueId().str());
    openDashboard(player);
}

void ExchangePlugin::openAllProducts(endstone::Player& player, std::size_t page) {
    auto& state = dashboardState(player);
    state.category_id.clear();
    state.search_query.clear();
    state.category_page = 0;
    state.page = page;
    state.product_key.clear();
    search_queries_.erase(player.getUniqueId().str());
    openDashboard(player);
}

void ExchangePlugin::openSearch(endstone::Player& player) {
    endstone::ModalForm form;
    form.setTitle(endstone::ColorFormat::Bold + endstone::ColorFormat::LightPurple + "搜索物品");
    form.addControl(endstone::TextInput(
        endstone::ColorFormat::Green + "输入物品名称或 ID",
        "diamond, stone, redstone, oak_log"));
    form.setSubmitButton(endstone::ColorFormat::Yellow + "搜索");
    form.setOnSubmit([this](endstone::Player* submitted, std::string json) {
        if (submitted == nullptr) {
            return;
        }
        const auto values = parseModalValues(json);
        if (values.empty() || trim(values[0]).empty()) {
            sendNotice(*submitted, "请输入搜索关键词。");
            openDashboard(*submitted);
            return;
        }
        auto& state = dashboardState(*submitted);
        state.search_query = trim(values[0]);
        state.category_id.clear();
        state.category_page = 0;
        state.page = 0;
        state.product_key.clear();
        search_queries_[submitted->getUniqueId().str()] = state.search_query;
        openDashboard(*submitted);
    });
    player.sendForm(std::move(form));
}

void ExchangePlugin::openSearchResults(endstone::Player& player, std::size_t page) {
    const auto it = search_queries_.find(player.getUniqueId().str());
    if (it == search_queries_.end() || it->second.empty()) {
        openSearch(player);
        return;
    }
    auto& state = dashboardState(player);
    state.search_query = it->second;
    state.category_id.clear();
    state.category_page = 0;
    state.page = page;
    state.product_key.clear();
    openDashboard(player);
}

void ExchangePlugin::openProduct(endstone::Player& player, const std::string& product_key) {
    openDashboardProduct(player, product_key);
}

void ExchangePlugin::openOrderBook(endstone::Player& player, const std::string& product_key) {
    auto product = repository_->getProduct(product_key);
    if (!product) {
        sendNotice(player, "商品不存在。");
        openDashboard(player);
        return;
    }
    sendForm(player, ui_.orderBookPage(*product, service_->listOrderBook(product_key, 12)));
}

void ExchangePlugin::openMyOrders(endstone::Player& player) {
    sendForm(player, ui_.myOrdersPage(service_->listPlayerOrders(playerRef(player), true)));
}

void ExchangePlugin::openMailbox(endstone::Player& player) {
    sendForm(player, ui_.mailboxPage(service_->listMailbox(playerRef(player))));
}

void ExchangePlugin::openAdmin(endstone::Player& player) {
    sendForm(player, ui_.adminHome(service_->getCatalog("building")));
}

void ExchangePlugin::openTradeForm(endstone::Player& player, ui::ActionKind action, const std::string& product_key) {
    auto product = repository_->getProduct(product_key);
    if (!product) {
        sendNotice(player, "商品不存在。");
        openDashboard(player);
        return;
    }

    const auto needs_price = action == ui::ActionKind::LimitBuy || action == ui::ActionKind::LimitSell;
    std::string title;
    switch (action) {
    case ui::ActionKind::MarketBuy:
        title = "市价买入";
        break;
    case ui::ActionKind::LimitBuy:
        title = "挂单买入";
        break;
    case ui::ActionKind::MarketSell:
        title = "市价卖出";
        break;
    case ui::ActionKind::LimitSell:
        title = "挂单卖出";
        break;
    default:
        title = "交易";
        break;
    }

    endstone::ModalForm form;
    form.setTitle(endstone::ColorFormat::Bold + endstone::ColorFormat::LightPurple + title + " - " + product->display_name);
    form.addControl(endstone::TextInput(
        endstone::ColorFormat::Green + "数量",
        "输入正整数",
        "1"));
    if (needs_price) {
        form.addControl(endstone::TextInput(
            endstone::ColorFormat::Green + "单价",
            "输入正整数",
            "1"));
    }
    form.setSubmitButton(endstone::ColorFormat::Yellow + "下一步");
    form.setOnSubmit([this, action, product_key, needs_price](endstone::Player* submitted, std::string json) {
        if (submitted == nullptr) {
            return;
        }
        const auto values = parseModalValues(json);
        if (values.empty()) {
            sendNotice(*submitted, "请输入数量。");
            openDashboardProduct(*submitted, product_key);
            return;
        }
        const auto quantity = parsePositiveInt32(values[0]);
        if (!quantity) {
            sendNotice(*submitted, "数量必须是正整数。");
            openDashboardProduct(*submitted, product_key);
            return;
        }
        std::int64_t unit_price = 0;
        if (needs_price) {
            if (values.size() < 2) {
                sendNotice(*submitted, "请输入单价。");
                openDashboardProduct(*submitted, product_key);
                return;
            }
            const auto price = parsePositiveInt64(values[1]);
            if (!price) {
                sendNotice(*submitted, "单价必须是正整数。");
                openDashboardProduct(*submitted, product_key);
                return;
            }
            unit_price = *price;
        }
        openTradeConfirm(*submitted, action, product_key, *quantity, unit_price);
    });
    player.sendForm(std::move(form));
}

void ExchangePlugin::openTradeConfirm(endstone::Player& player, ui::ActionKind action, const std::string& product_key, std::int32_t quantity, std::int64_t unit_price) {
    auto product = repository_->getProduct(product_key);
    if (!product) {
        sendNotice(player, "商品不存在。");
        openDashboard(player);
        return;
    }
    std::string action_name;
    switch (action) {
    case ui::ActionKind::MarketBuy:
        action_name = "市价买入";
        break;
    case ui::ActionKind::LimitBuy:
        action_name = "挂单买入";
        break;
    case ui::ActionKind::MarketSell:
        action_name = "市价卖出";
        break;
    case ui::ActionKind::LimitSell:
        action_name = "挂单卖出";
        break;
    default:
        action_name = "交易";
        break;
    }

    endstone::ActionForm form;
    std::ostringstream body;
    body << endstone::ColorFormat::Green << "商品: " << endstone::ColorFormat::White << product->display_name
         << "\n" << endstone::ColorFormat::Green << "物品ID: " << endstone::ColorFormat::White << product->item_id
         << "\n" << endstone::ColorFormat::Green << "数量: " << endstone::ColorFormat::White << quantity;
    if (unit_price > 0) {
        body << "\n" << endstone::ColorFormat::Green << "单价: " << endstone::ColorFormat::White << unit_price;
    }
    form.setTitle(endstone::ColorFormat::Bold + endstone::ColorFormat::LightPurple + "确认表单")
        .setContent(body.str());
    form.addButton(endstone::ColorFormat::Yellow + "确认 " + action_name, "textures/ui/check",
                   [this, payload = tradePayload(action, product_key, quantity, unit_price)](endstone::Player* clicked) {
                       if (clicked != nullptr) {
                           executeConfirmedTrade(*clicked, payload);
                       }
                   });
    form.addButton(endstone::ColorFormat::Yellow + "返回", "textures/ui/refresh_light",
                   [this, product_key](endstone::Player* clicked) {
                       if (clicked != nullptr) {
                           openDashboardProduct(*clicked, product_key);
                       }
                   });
    player.sendForm(std::move(form));
}

void ExchangePlugin::sendForm(endstone::Player& player, const ui::FormSpec& spec) {
    endstone::ActionForm form;
    form.setTitle(spec.title).setContent(spec.body);
    const auto add_button = [this, &form](const ui::ButtonSpec& button) {
        form.addButton(button.text, button.icon.empty() ? std::nullopt : std::optional<std::string>{button.icon},
                       [this, button](endstone::Player* clicked) {
                           if (clicked != nullptr) {
                               handleAction(*clicked, button);
                           }
                       });
    };
    if (!spec.controls.empty()) {
        for (const auto& control : spec.controls) {
            switch (control.kind) {
            case ui::ControlKind::Button:
                add_button(control.button);
                break;
            case ui::ControlKind::Header:
                form.addHeader(control.text);
                break;
            case ui::ControlKind::Label:
                form.addLabel(control.text);
                break;
            case ui::ControlKind::Divider:
                form.addDivider();
                break;
            }
        }
    } else {
        for (const auto& button : spec.buttons) {
            add_button(button);
        }
    }
    player.sendForm(std::move(form));
}

void ExchangePlugin::handleAction(endstone::Player& player, const ui::ButtonSpec& button) {
    try {
        switch (button.action) {
        case ui::ActionKind::Noop:
            openDashboard(player);
            return;
        case ui::ActionKind::OpenDashboard:
            openDashboard(player);
            return;
        case ui::ActionKind::DashboardCategory:
            openDashboardCategory(player, button.target);
            return;
        case ui::ActionKind::DashboardCategoryPage: {
            const auto page = parsePageIndex(button.target);
            if (!page) {
                sendNotice(player, "分类页码无效。");
                openDashboard(player);
                return;
            }
            openDashboardCategoryPage(player, *page);
            return;
        }
        case ui::ActionKind::DashboardProduct:
            openDashboardProduct(player, button.target);
            return;
        case ui::ActionKind::DashboardPage: {
            const auto page = parsePageIndex(button.target);
            if (!page) {
                sendNotice(player, "页码无效。");
                openDashboard(player);
                return;
            }
            openDashboardPage(player, *page);
            return;
        }
        case ui::ActionKind::OpenAllProducts:
            resetDashboard(player);
            return;
        case ui::ActionKind::OpenSearch:
            openSearch(player);
            return;
        case ui::ActionKind::OpenCategory:
            openDashboardCategory(player, button.target);
            return;
        case ui::ActionKind::OpenProduct:
            openDashboardProduct(player, button.target);
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
            openDashboardProduct(player, button.target);
            return;
        }
        case ui::ActionKind::MarketBuy:
        case ui::ActionKind::LimitBuy:
        case ui::ActionKind::MarketSell:
        case ui::ActionKind::LimitSell:
            if (!looksLikeProductKey(button.target)) {
                sendNotice(player, "请先从商品页选择交易方向。");
                return;
            }
            openTradeForm(player, button.action, button.target);
            return;
        case ui::ActionKind::ConfirmTrade:
            executeConfirmedTrade(player, button.target);
            return;
        case ui::ActionKind::OpenInventorySell:
        case ui::ActionKind::SellAll:
            sendNotice(player, "请先选择具体物品，再在商品面板卖出。");
            return;
        case ui::ActionKind::OpenMyOrders:
            openMyOrders(player);
            return;
        case ui::ActionKind::OpenMailbox:
            openMailbox(player);
            return;
        case ui::ActionKind::CancelOrder: {
            const auto order_id = parsePositiveInt64(button.target);
            if (!order_id) {
                sendNotice(player, "订单 ID 无效。");
                return;
            }
            const auto order = service_->cancelOrder(playerRef(player), *order_id);
            sendNotice(player, "已取消订单 #" + std::to_string(order.id) + "。");
            openMyOrders(player);
            return;
        }
        case ui::ActionKind::ClaimMailboxItem: {
            const auto mailbox_id = parsePositiveInt64(button.target);
            if (!mailbox_id) {
                sendNotice(player, "邮箱物品 ID 无效。");
                return;
            }
            claimMailboxItem(player, *mailbox_id);
            return;
        }
        case ui::ActionKind::ClaimAllMailbox:
            claimAllMailbox(player);
            return;
        case ui::ActionKind::Back:
            if (looksLikeProductKey(button.target)) {
                openDashboardProduct(player, button.target);
            } else {
                openDashboard(player);
            }
            return;
        case ui::ActionKind::NextPage:
        case ui::ActionKind::PrevPage: {
            const auto [target, page] = parsePageTarget(button.target);
            if (target == "search") {
                openDashboardPage(player, page);
            } else if (target == "all") {
                openAllProducts(player, page);
            } else {
                auto& state = dashboardState(player);
                state.category_id = target;
                state.search_query.clear();
                state.page = page;
                state.product_key.clear();
                openDashboard(player);
            }
            return;
        }
        }
    } catch (const ExchangeException& exc) {
        sendNotice(player, std::string("交易失败: ") + exc.what());
    } catch (const std::exception& exc) {
        sendNotice(player, std::string("交易所错误: ") + exc.what());
    }
}

void ExchangePlugin::executeConfirmedTrade(endstone::Player& player, const std::string& payload) {
    const auto parts = split(payload, '|');
    if (parts.size() != 4) {
        sendNotice(player, "交易参数无效。");
        openDashboard(player);
        return;
    }
    const auto action = actionFromToken(parts[0]);
    const auto quantity = parsePositiveInt32(parts[2]);
    const auto unit_price = parsePositiveInt64(parts[3]);
    if (!action || !quantity) {
        sendNotice(player, "交易参数无效。");
        openDashboard(player);
        return;
    }

    const auto product = repository_->getProduct(parts[1]);
    if (!product) {
        sendNotice(player, "商品不存在。");
        openDashboard(player);
        return;
    }

    const auto ref = playerRef(player);
    switch (*action) {
    case ui::ActionKind::MarketBuy: {
        const auto order = service_->placeMarketBuy(ref, product->product_key, *quantity);
        sendNotice(player, "市价买入完成，数量 " + std::to_string(order.original_quantity) + "，物品已进入交易所邮箱。");
        openDashboardProduct(player, product->product_key);
        return;
    }
    case ui::ActionKind::LimitBuy: {
        if (!unit_price) {
            sendNotice(player, "缺少买单单价。");
            openDashboardProduct(player, product->product_key);
            return;
        }
        const auto order = service_->placeLimitBuy(ref, product->product_key, *quantity, *unit_price);
        sendNotice(player, "已提交买单 #" + std::to_string(order.id) + "。");
        openDashboardProduct(player, product->product_key);
        return;
    }
    case ui::ActionKind::MarketSell:
    case ui::ActionKind::LimitSell: {
        if (!removeInventoryItems(player, *product, *quantity)) {
            openDashboardProduct(player, product->product_key);
            return;
        }
        try {
            Order order;
            if (*action == ui::ActionKind::MarketSell) {
                order = service_->placeMarketSell(ref, product->product_key, {snapshotFor(*product, *quantity)});
                sendNotice(player, "市价卖出完成，订单 #" + std::to_string(order.id) + "。");
            } else {
                if (!unit_price) {
                    throw InvalidOrder("missing sell price");
                }
                order = service_->placeLimitSell(ref, product->product_key, {snapshotFor(*product, *quantity)}, *unit_price);
                sendNotice(player, "已提交卖单 #" + std::to_string(order.id) + "。");
            }
            openDashboardProduct(player, product->product_key);
        } catch (...) {
            try {
                player.getInventory().addItem({endstone::ItemStack(endstone::ItemTypeId(product->item_id), *quantity)});
            } catch (...) {
                sendNotice(player, "交易失败且物品退回失败，请联系管理员。");
            }
            throw;
        }
        return;
    }
    default:
        sendNotice(player, "交易类型无效。");
        openDashboard(player);
        return;
    }
}

void ExchangePlugin::claimMailboxItem(endstone::Player& player, std::int64_t mailbox_id) {
    const auto item = repository_->getMailboxItem(mailbox_id);
    if (!item || item->player_uuid != player.getUniqueId().str() || item->claimed) {
        sendNotice(player, "邮箱物品不存在或已领取。");
        openMailbox(player);
        return;
    }
    if (!giveMailboxItem(player, *item)) {
        openMailbox(player);
        return;
    }
    const auto claimed = service_->markMailboxClaimed(playerRef(player), mailbox_id);
    sendNotice(player, "已领取邮箱物品 #" + std::to_string(claimed.id) + "，数量 " + std::to_string(claimed.quantity) + "。");
    openMailbox(player);
}

void ExchangePlugin::claimAllMailbox(endstone::Player& player) {
    auto items = service_->listMailbox(playerRef(player));
    std::int32_t claimed_count = 0;
    for (const auto& item : items) {
        if (!giveMailboxItem(player, item)) {
            break;
        }
        service_->markMailboxClaimed(playerRef(player), item.id);
        ++claimed_count;
    }
    sendNotice(player, "已领取 " + std::to_string(claimed_count) + " 个邮箱条目。");
    openMailbox(player);
}

ItemSnapshot ExchangePlugin::snapshotFor(const Product& product, std::int32_t quantity) const {
    return ItemSnapshot{product.product_key, product.item_id, product.enchants, quantity, {}, product.item_id};
}

bool ExchangePlugin::removeInventoryItems(endstone::Player& player, const Product& product, std::int32_t quantity) {
    try {
        auto& inventory = player.getInventory();
        if (!inventory.containsAtLeast(product.item_id, quantity)) {
            sendNotice(player, "背包内没有足够的 " + product.display_name + "。");
            return false;
        }
        auto leftovers = inventory.removeItem({endstone::ItemStack(endstone::ItemTypeId(product.item_id), quantity)});
        if (!leftovers.empty()) {
            sendNotice(player, "背包扣除失败，请重新检查物品数量。");
            return false;
        }
        return true;
    } catch (const std::exception& exc) {
        sendNotice(player, std::string("背包扣除失败: ") + exc.what());
        return false;
    }
}

bool ExchangePlugin::giveMailboxItem(endstone::Player& player, const MailboxItem& item) {
    const auto product = repository_->getProduct(item.product_key);
    if (!product) {
        sendNotice(player, "邮箱中的商品不存在，无法领取。");
        return false;
    }
    try {
        auto leftovers = player.getInventory().addItem({endstone::ItemStack(endstone::ItemTypeId(product->item_id), item.quantity)});
        if (!leftovers.empty()) {
            sendNotice(player, "背包空间不足，领取已暂停。");
            return false;
        }
        return true;
    } catch (const std::exception& exc) {
        sendNotice(player, std::string("领取失败: ") + exc.what());
        return false;
    }
}

void ExchangePlugin::sendNotice(endstone::Player& player, const std::string& message) {
    player.sendMessage("[交易所] " + message);
}

std::vector<ui::CategorySpec> ExchangePlugin::categories() const {
    std::vector<ui::CategorySpec> out;
    for (const auto& category : catalog::categories()) {
        out.push_back({category.id, category.name, category.icon});
    }
    return out;
}

DashboardState& ExchangePlugin::dashboardState(endstone::Player& player) {
    return dashboard_states_[player.getUniqueId().str()];
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
