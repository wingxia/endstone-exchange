#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <endstone/endstone.hpp>

#include "endstone_exchange/core/Economy.hpp"
#include "endstone_exchange/core/ExchangeService.hpp"
#include "endstone_exchange/core/ItemIdentity.hpp"
#include "endstone_exchange/core/Repository.hpp"
#include "endstone_exchange/ui/ExchangeUiModel.hpp"

namespace exchange::plugin {

struct DashboardState {
    std::string category_id;
    std::size_t page{0};
    std::string product_key;
    std::string search_query;
};

class ExchangePlugin : public endstone::Plugin {
public:
    void onEnable() override;
    void onDisable() override;
    bool onCommand(endstone::CommandSender& sender, const endstone::Command& command, const std::vector<std::string>& args) override;

private:
    void seedCatalog();
    void openHome(endstone::Player& player);
    void openDashboard(endstone::Player& player);
    void openDashboardCategory(endstone::Player& player, const std::string& category_id);
    void openDashboardProduct(endstone::Player& player, const std::string& product_key);
    void openDashboardPage(endstone::Player& player, std::size_t page);
    void resetDashboard(endstone::Player& player);
    void openCategory(endstone::Player& player, const std::string& category_id, std::size_t page = 0);
    void openAllProducts(endstone::Player& player, std::size_t page = 0);
    void openSearch(endstone::Player& player);
    void openSearchResults(endstone::Player& player, std::size_t page = 0);
    void openProduct(endstone::Player& player, const std::string& product_key);
    void openOrderBook(endstone::Player& player, const std::string& product_key);
    void openMyOrders(endstone::Player& player);
    void openMailbox(endstone::Player& player);
    void openAdmin(endstone::Player& player);
    void openTradeForm(endstone::Player& player, ui::ActionKind action, const std::string& product_key);
    void openTradeConfirm(endstone::Player& player, ui::ActionKind action, const std::string& product_key, std::int32_t quantity, std::int64_t unit_price);
    void sendForm(endstone::Player& player, const ui::FormSpec& spec);
    void handleAction(endstone::Player& player, const ui::ButtonSpec& button);
    void executeConfirmedTrade(endstone::Player& player, const std::string& payload);
    void claimMailboxItem(endstone::Player& player, std::int64_t mailbox_id);
    void claimAllMailbox(endstone::Player& player);
    void sendNotice(endstone::Player& player, const std::string& message);
    std::vector<ui::CategorySpec> categories() const;
    DashboardState& dashboardState(endstone::Player& player);
    PlayerRef playerRef(endstone::Player& player) const;
    ItemSnapshot snapshotFor(const Product& product, std::int32_t quantity) const;
    bool removeInventoryItems(endstone::Player& player, const Product& product, std::int32_t quantity);
    bool giveMailboxItem(endstone::Player& player, const MailboxItem& item);

    std::unique_ptr<InMemoryRepository> repository_;
    std::unique_ptr<Economy> economy_;
    std::unique_ptr<ExchangeService> service_;
    ui::ExchangeUiModel ui_{18};
    std::unordered_map<std::string, std::string> search_queries_;
    std::unordered_map<std::string, DashboardState> dashboard_states_;
};

}  // namespace exchange::plugin
