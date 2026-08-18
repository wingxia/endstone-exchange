#pragma once

#include "endstone_exchange/config.hpp"
#include "endstone_exchange/domain.hpp"
#include "endstone_exchange/exchange_service.hpp"
#include "endstone_exchange/interaction_gate.hpp"
#include "endstone_exchange/localization.hpp"
#include "endstone_exchange/market_display.hpp"
#include "endstone_exchange/trade_form.hpp"
#include "endstone_exchange/umoney_gateway.hpp"

#include <endstone/endstone.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace exchange {

struct HologramSnapshotState;

struct FrameAddress {
    std::string target_key;
    std::string dimension_name;
    int x{0};
    int y{0};
    int z{0};
};

class ExchangePlugin : public endstone::Plugin {
  public:
    void onEnable() override;
    void onDisable() override;
    bool onCommand(endstone::CommandSender &sender, const endstone::Command &command,
                   const std::vector<std::string> &args) override;

    void onPlayerInteract(endstone::PlayerInteractEvent &event);
    void onPlayerInteractActor(endstone::PlayerInteractActorEvent &event);
    void onBlockBreak(endstone::BlockBreakEvent &event);
    void onProtectedBlockBreak(endstone::BlockBreakEvent &event);
    void onBlockPlace(endstone::BlockPlaceEvent &event);
    void onActorExplode(endstone::ActorExplodeEvent &event);
    void onBlockExplode(endstone::BlockExplodeEvent &event);
    void onActorDamage(endstone::ActorDamageEvent &event);
    void onActorRemove(endstone::ActorRemoveEvent &event);
    void onPlayerJoin(endstone::PlayerJoinEvent &event);
    void onPlayerQuit(endstone::PlayerQuitEvent &event);
    void onPlayerMove(endstone::PlayerMoveEvent &event);
    void onChunkLoad(endstone::ChunkLoadEvent &event);
    void onChunkUnload(endstone::ChunkUnloadEvent &event);
    void onPlayerDropItem(endstone::PlayerDropItemEvent &event);
    void onPlayerPickupItem(endstone::PlayerPickupItemEvent &event);

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
    std::unordered_map<std::string, Id> frame_listing_index_;
    std::unordered_set<Id> pending_frame_settlements_;
    InteractionGate interaction_gate_{std::chrono::milliseconds(750)};
    std::unordered_set<std::string> open_trade_forms_;
    std::unordered_set<std::string> open_frame_forms_;
    std::unordered_set<std::string> pending_join_hologram_recreates_;
    std::unordered_map<std::string, std::unordered_set<std::string>> join_loaded_chunks_;
    std::unordered_set<Id> pending_hologram_recreates_;
    std::shared_ptr<endstone::Task> refresh_task_;
    std::shared_ptr<endstone::Task> economy_task_;
    std::shared_ptr<endstone::Task> frame_recovery_task_;
    std::shared_ptr<HologramSnapshotState> hologram_snapshot_state_;
    std::unique_ptr<UmoneyGateway> umoney_gateway_;
    bool processing_economy_{false};
    bool ready_{false};

    [[nodiscard]] bool isExchanger(const std::optional<endstone::ItemStack> &item) const;
    [[nodiscard]] bool isPriceStick(const std::optional<endstone::ItemStack> &item) const;
    [[nodiscard]] bool canAdmin(endstone::Player &player) const;
    [[nodiscard]] bool isProtectedBlock(const endstone::Block &block) const noexcept;
    [[nodiscard]] std::string blockTargetKey(const endstone::Block &block) const;
    [[nodiscard]] std::string actorTargetKey(const endstone::Actor &actor) const;
    [[nodiscard]] std::optional<Id> marketIdForActor(const endstone::Actor &actor) const;
    [[nodiscard]] std::optional<ItemPrototype> prototypeForBlock(endstone::Block &block);
    [[nodiscard]] std::optional<ItemPrototype> prototypeForActor(endstone::Actor &actor);
    [[nodiscard]] bool acceptInteraction(const endstone::Player &player);
    [[nodiscard]] FrameAddress frameAddress(const endstone::Block &block) const;
    [[nodiscard]] static bool sameItem(const ItemPrototype &left, const ItemPrototype &right) noexcept;

    void toggleBlock(endstone::Player &player, endstone::Block &block);
    void queueItemFrameToggle(endstone::Player &player, endstone::Block &block);
    using FrameCaptureCallback =
        std::function<void(endstone::Player *, std::optional<ItemPrototype>, std::exception_ptr)>;
    void queueItemFrameCapture(
        endstone::Player *player, FrameAddress address, FrameCaptureCallback callback, bool announce = true);
    void completeItemFrameCapture(FrameAddress address, std::string structure_name, std::string level_name,
                                  std::string player_uuid, std::string player_name,
                                  FrameCaptureCallback callback, int attempt);
    void handlePriceStick(endstone::Player &player, endstone::Block &block);
    void queueFramePriceCapture(endstone::Player &player, FrameAddress address,
                                std::optional<FrameListing> existing = std::nullopt);
    void openFrameManagementForm(endstone::Player &player, const FrameListing &listing);
    void openFramePriceForm(endstone::Player &player, FrameAddress address, ItemPrototype item,
                            std::optional<FrameListing> existing = std::nullopt);
    void queueFramePurchaseReview(endstone::Player &player, const FrameListing &listing);
    void openFramePurchaseReview(endstone::Player &player, const FrameListing &listing);
    void queueFramePurchase(endstone::Player &player, const FrameListing &listing);
    void settleFrameListing(const FrameListing &listing);
    void verifyFrameClearedAndDrop(const FrameListing &listing);
    void spawnFrameSaleDrop(const FrameListing &listing);
    [[nodiscard]] endstone::Item *findFrameSaleDrop(Id listing_id) const;
    void recoverFrameListings();
    void restoreFrameListingIndex();
    void reconcileFrameSaleItems(endstone::Player &player);
    void protectBlocks(std::vector<std::unique_ptr<endstone::Block>> &blocks) const;
    void toggleActor(endstone::Player &player, endstone::Actor &actor);
    void deactivate(endstone::Player *player, Id market_id, std::string_view reason, bool preserve_orders);
    void indexMarket(const Market &market);
    void unindexMarket(Id market_id);
    void restoreMarkets();

    void queueTradeForm(endstone::Player &player, Id market_id);
    void openTradeForm(endstone::Player &player, Id market_id);
    void queueTradeInputForm(endstone::Player &player, Id market_id, TradeDraft draft);
    void openTradeInputForm(endstone::Player &player, Id market_id, TradeDraft draft);
    void queueTradeReviewForm(endstone::Player &player, Id market_id, TradeDraft draft);
    void openTradeReviewForm(endstone::Player &player, Id market_id, TradeDraft draft);
    void queueTradeSubmission(endstone::Player &player, Id market_id, TradeDraft draft);
    void submitTradeDraft(endstone::Player &player, Id market_id, const TradeDraft &draft);
    void openOrdersForm(endstone::Player &player);
    [[nodiscard]] ExecutionResult submitSellEscrow(endstone::Player &player, const OrderRequest &request);
    void reconcileInternalEscrowMarkers(endstone::Player &player);
    void reconcileSellEscrows(endstone::Player &player);
    [[nodiscard]] int claimDeliveries(endstone::Player &player, bool announce = true);
    void queueInventoryResync(endstone::Player &player);
    void giveExchanger(endstone::Player &player);
    void localizeExchangers(endstone::Player &player);
    void processEconomyTransfers();
    [[nodiscard]] bool processEconomyTransfer(const EconomyTransfer &transfer, bool throw_on_failure = false);
    [[nodiscard]] Cents spendableBalance(std::string_view player_uuid, std::string_view player_name);
    void fundDirectBalance(std::string_view player_uuid, std::string_view player_name, Cents required_cents,
                           Language language);
    void sweepDirectBalances();

    void refreshHolograms();
    void refreshHologram(const Market &market);
    void refreshHologram(const Market &market, const OrderBook &book);
    void sendHologramAppearance(const endstone::Actor &hologram, const Market &market, const OrderBook &book,
                                endstone::Player *recipient = nullptr) const;
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
    [[nodiscard]] Language languageFor(const endstone::CommandSender &sender) const noexcept;
    [[nodiscard]] std::string localizedItemName(const ItemPrototype &item, Language language) const;

    [[nodiscard]] static std::string formatMoney(Cents cents);
    [[nodiscard]] static std::string targetTag(Id market_id);
    [[nodiscard]] static std::string hologramTag(Id market_id);
    [[nodiscard]] static std::string chunkKey(std::string_view dimension_name, int chunk_x, int chunk_z);
    [[nodiscard]] static std::optional<Id> marketIdFromHologramTag(std::string_view tag);
    [[nodiscard]] static bool hasTag(const endstone::Actor &actor, std::string_view tag);
};

} // namespace exchange
