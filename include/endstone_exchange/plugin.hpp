#pragma once

#include "endstone_exchange/config.hpp"
#include "endstone_exchange/domain.hpp"
#include "endstone_exchange/exchange_service.hpp"
#include "endstone_exchange/price_window.hpp"

#include <endstone/endstone.hpp>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace exchange {

struct HologramSnapshotState;

class ExchangePlugin : public endstone::Plugin {
  public:
    void onEnable() override;
    void onDisable() override;
    bool onCommand(endstone::CommandSender &sender, const endstone::Command &command,
                   const std::vector<std::string> &args) override;

    void onPlayerInteract(endstone::PlayerInteractEvent &event);
    void onPlayerInteractActor(endstone::PlayerInteractActorEvent &event);
    void onBlockBreak(endstone::BlockBreakEvent &event);
    void onActorDamage(endstone::ActorDamageEvent &event);
    void onActorRemove(endstone::ActorRemoveEvent &event);
    void onPlayerJoin(endstone::PlayerJoinEvent &event);
    void onPlayerDropItem(endstone::PlayerDropItemEvent &event);

  private:
    Config config_;
    std::unique_ptr<Database> database_;
    std::unique_ptr<ExchangeService> service_;
    std::unordered_map<Id, Market> markets_;
    std::unordered_map<std::string, Id> target_index_;
    std::unordered_map<Id, std::int64_t> hologram_ids_;
    std::unordered_map<Id, OrderBook> hologram_books_;
    std::unordered_map<std::string, std::string> pending_frame_captures_;
    std::shared_ptr<endstone::Task> refresh_task_;
    std::shared_ptr<HologramSnapshotState> hologram_snapshot_state_;
    bool ready_{false};

    [[nodiscard]] bool isExchanger(const std::optional<endstone::ItemStack> &item) const;
    [[nodiscard]] bool canAdmin(endstone::Player &player) const;
    [[nodiscard]] std::string blockTargetKey(const endstone::Block &block) const;
    [[nodiscard]] std::string actorTargetKey(const endstone::Actor &actor) const;
    [[nodiscard]] std::optional<Id> marketIdForActor(const endstone::Actor &actor) const;
    [[nodiscard]] std::optional<ItemPrototype> prototypeForBlock(endstone::Block &block);
    [[nodiscard]] std::optional<ItemPrototype> prototypeForActor(endstone::Actor &actor);
    [[nodiscard]] endstone::ItemStack makeItemStack(const ItemPrototype &prototype, int amount) const;

    void toggleBlock(endstone::Player &player, endstone::Block &block);
    void queueItemFrameToggle(endstone::Player &player, endstone::Block &block);
    void toggleActor(endstone::Player &player, endstone::Actor &actor);
    void deactivate(endstone::Player *player, Id market_id, std::string_view reason, bool preserve_orders);
    void indexMarket(const Market &market);
    void unindexMarket(Id market_id);
    void restoreMarkets();

    void openTradeForm(endstone::Player &player, Id market_id);
    void submitTradeForm(endstone::Player &player, Id market_id, const PriceSliderWindow &price_window,
                         std::string_view response);
    void openOrdersForm(endstone::Player &player);
    [[nodiscard]] ExecutionResult submitSellEscrow(endstone::Player &player, const OrderRequest &request);
    void reconcileSellEscrows(endstone::Player &player);
    void claimDeliveries(endstone::Player &player);
    void giveExchanger(endstone::Player &player);

    void refreshHolograms();
    void refreshHologram(const Market &market);
    void refreshHologram(const Market &market, const OrderBook &book);
    void removeHologram(Id market_id);
    void removeAllHolograms();
    [[nodiscard]] endstone::Actor *findActorById(std::int64_t actor_id) const;
    [[nodiscard]] endstone::Actor *findActorByTag(std::string_view tag) const;
    [[nodiscard]] std::optional<endstone::Location> targetLocation(const Market &market) const;
    [[nodiscard]] std::string hologramText(const Market &market, const OrderBook &book) const;

    [[nodiscard]] static std::vector<double> numericFormValues(std::string_view response);
    [[nodiscard]] static std::string formatMoney(Cents cents);
    [[nodiscard]] static std::string targetTag(Id market_id);
    [[nodiscard]] static std::string hologramTag(Id market_id);
    [[nodiscard]] static bool hasTag(const endstone::Actor &actor, std::string_view tag);
};

} // namespace exchange
