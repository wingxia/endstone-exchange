#pragma once

#include <memory>
#include <string>
#include <vector>

#include <endstone/endstone.hpp>

#include "endstone_exchange/core/Economy.hpp"
#include "endstone_exchange/core/ExchangeService.hpp"
#include "endstone_exchange/core/ItemIdentity.hpp"
#include "endstone_exchange/core/Repository.hpp"
#include "endstone_exchange/ui/ExchangeUiModel.hpp"

namespace exchange::plugin {

class ExchangePlugin : public endstone::Plugin {
public:
    void onEnable() override;
    void onDisable() override;
    bool onCommand(endstone::CommandSender& sender, const endstone::Command& command, const std::vector<std::string>& args) override;

private:
    void seedCatalog();
    void openHome(endstone::Player& player);
    void openCategory(endstone::Player& player, const std::string& category_id, std::size_t page = 0);
    void openProduct(endstone::Player& player, const std::string& product_key);
    void openOrderBook(endstone::Player& player, const std::string& product_key);
    void openAdmin(endstone::Player& player);
    void sendForm(endstone::Player& player, const ui::FormSpec& spec);
    void handleAction(endstone::Player& player, const ui::ButtonSpec& button);
    void sendNotice(endstone::Player& player, const std::string& message);
    std::vector<ui::CategorySpec> categories() const;
    PlayerRef playerRef(endstone::Player& player) const;

    std::unique_ptr<InMemoryRepository> repository_;
    std::unique_ptr<Economy> economy_;
    std::unique_ptr<ExchangeService> service_;
    ui::ExchangeUiModel ui_{18};
};

}  // namespace exchange::plugin
