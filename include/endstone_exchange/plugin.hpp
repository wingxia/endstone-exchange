#pragma once

#include "endstone_exchange/config.hpp"
#include "endstone_exchange/domain.hpp"
#include "endstone_exchange/exchange_service.hpp"
#include "endstone_exchange/interaction_gate.hpp"
#include "endstone_exchange/market_display.hpp"
#include "endstone_exchange/price_window.hpp"

#include <endstone/endstone.hpp>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    void onPlayerQuit(endstone::PlayerQuitEvent &event);
    void onPlayerMove(endstone::PlayerMoveEvent &event);
    void onChunkLoad(endstone::ChunkLoadEvent &event);
    void onChunkUnload(endstone::ChunkUnloadEvent &event);
    void onPlayerDropItem(endstone::PlayerDropItemEvent &event);

  private:
    Config config_;
    std::unique_ptr<Database> database_;
    std::unique_ptr<ExchangeService> service_;
    std::unordered_map<Id, Market> markets_;
    std::unordered_map<std::string, Id> target_index_;
    std::unordered_map<Id, std::int64_t> hologram_ids_;
    std::unordered_map<Id, OrderBook> hologram_books_;
    std::unordered_map<Id, HologramAnchor> hologram_anchors_;
    std::unordered_set<std::string> loaded_chunks_;
    std::unordered_set<std::string> hologram_spawn_ready_chunks_;
    std::unordered_map<std::string, std::string> pending_frame_captures_;
    InteractionGate interaction_gate_{std::chrono::milliseconds(750)};
    std::unordered_set<std::string> open_trade_forms_;
    std::unordered_set<std::string> pending_join_hologram_recreates_;
    std::unordered_map<std::string, std::unordered_set<std::string>> join_loaded_chunks_;
    std::unordered_set<Id> pending_hologram_recreates_;
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
    [[nodiscard]] bool acceptInteraction(const endstone::Player &player);

    void toggleBlock(endstone::Player &player, endstone::Block &block);
    void queueItemFrameToggle(endstone::Player &player, endstone::Block &block);
    void toggleActor(endstone::Player &player, endstone::Actor &actor);
    void deactivate(endstone::Player *player, Id market_id, std::string_view reason, bool preserve_orders);
    void indexMarket(const Market &market);
    void unindexMarket(Id market_id);
    void restoreMarkets();

    void queueTradeForm(endstone::Player &player, Id market_id);
    void openTradeForm(endstone::Player &player, Id market_id);
    void submitTradeForm(endstone::Player &player, Id market_id, const PriceSliderWindow &price_window,
                         std::string_view response);
    void openOrdersForm(endstone::Player &player);
    [[nodiscard]] ExecutionResult submitSellEscrow(endstone::Player &player, const OrderRequest &request);
    void reconcileInternalEscrowMarkers(endstone::Player &player);
    void reconcileSellEscrows(endstone::Player &player);
    [[nodiscard]] int claimDeliveries(endstone::Player &player, bool announce = true);
    void queueInventoryResync(endstone::Player &player);
    void giveExchanger(endstone::Player &player);

    void refreshHolograms();
    void refreshHologram(const Market &market);
    void refreshHologram(const Market &market, const OrderBook &book);
    void sendHologramAppearance(const endstone::Actor &hologram, endstone::Player *recipient = nullptr) const;
    void queuePlayerHologramSync(endstone::Player &player, bool recreate_if_pending);
    void syncHologramsForPlayer(endstone::Player &player) const;
    [[nodiscard]] bool recreateHologramsNearPlayer(endstone::Player &player);
    void queueLoadedChunkReconcile(std::string dimension_name, int chunk_x, int chunk_z);
    void reconcileLoadedChunk(std::string_view dimension_name, int chunk_x, int chunk_z);
    void removeHologram(Id market_id);
    void removeAllHolograms();
    [[nodiscard]] endstone::Actor *findActorById(std::int64_t actor_id) const;
    [[nodiscard]] endstone::Actor *findActorByTag(std::string_view tag) const;
    [[nodiscard]] std::optional<endstone::Location> targetLocation(const Market &market) const;
    [[nodiscard]] bool isTargetChunkLoaded(const Market &market) const;
    [[nodiscard]] bool isTargetChunkSpawnReady(const Market &market) const;
    [[nodiscard]] bool marketTargetsChunk(const Market &market, std::string_view dimension_name, int chunk_x,
                                          int chunk_z) const;

    [[nodiscard]] static std::vector<double> numericFormValues(std::string_view response);
    [[nodiscard]] static std::string formatMoney(Cents cents);
    [[nodiscard]] static std::string targetTag(Id market_id);
    [[nodiscard]] static std::string hologramTag(Id market_id);
    [[nodiscard]] static std::string chunkKey(std::string_view dimension_name, int chunk_x, int chunk_z);
    [[nodiscard]] static std::optional<Id> marketIdFromHologramTag(std::string_view tag);
    [[nodiscard]] static bool hasTag(const endstone::Actor &actor, std::string_view tag);
};

} // namespace exchange
