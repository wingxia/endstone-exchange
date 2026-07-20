#include "endstone_exchange/plugin.hpp"

#include "endstone_exchange/hologram_packet.hpp"
#include "endstone_exchange/inventory_escrow.hpp"
#include "endstone_exchange/item_identity.hpp"
#include "endstone_exchange/nbt_codec.hpp"
#include "endstone_exchange/structure_reader.hpp"
#include "endstone_exchange/trade_form.hpp"
#include "endstone_exchange/version.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <limits>
#include <mutex>
#include <numeric>
#include <set>
#include <string_view>
#include <thread>
#include <variant>

namespace exchange {

struct HologramSnapshotState {
    std::mutex mutex;
    std::optional<std::unordered_map<Id, OrderBook>> ready_snapshot;
    std::optional<std::string> error;
    std::atomic_bool query_running{false};
    std::atomic_bool stopping{false};
    std::jthread query_thread;
};

namespace {

constexpr std::string_view TargetTagPrefix = "exchange_target_";
constexpr std::string_view HologramTagPrefix = "exchange_hologram_";

bool isItemFrameBlock(const std::string_view block_type) {
    return block_type == "minecraft:frame" || block_type == "minecraft:glow_frame" ||
           block_type == "minecraft:item_frame" || block_type == "minecraft:glow_item_frame";
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
    return value;
}

std::string stripFormatting(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto byte = static_cast<unsigned char>(value[index]);
        if (byte == 0xc2 && index + 2 < value.size() && static_cast<unsigned char>(value[index + 1]) == 0xa7) {
            index += 2;
            continue;
        }
        if (byte == 0xa7 && index + 1 < value.size()) {
            ++index;
            continue;
        }
        result.push_back(value[index]);
    }
    while (!result.empty() && std::isspace(static_cast<unsigned char>(result.front()))) {
        result.erase(result.begin());
    }
    while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back()))) {
        result.pop_back();
    }
    return result;
}

std::string friendlyItemName(const endstone::ItemStack &item) {
    if (const auto meta = item.getItemMeta(); meta && meta->hasDisplayName()) {
        const auto name = stripFormatting(meta->getDisplayName());
        if (!name.empty()) {
            return name;
        }
    }
    std::string type = item.getType().getId();
    if (const auto colon = type.find(':'); colon != std::string::npos) {
        type.erase(0, colon + 1);
    }
    std::replace(type.begin(), type.end(), '_', ' ');
    return type;
}

} // namespace

void ExchangePlugin::onEnable() {
    const auto config_path = getDataFolder() / "config.toml";
    try {
        if (!std::filesystem::exists(config_path)) {
            Config::writeTemplate(config_path);
            getLogger().error("Created {}. Fill in database.password, then reload the plugin.", config_path.string());
            return;
        }
        config_ = Config::load(config_path);
        if (config_.database.password.empty()) {
            getLogger().error("database.password is empty in {}", config_path.string());
            return;
        }
        database_ = std::make_unique<Database>(config_.database);
        database_->connect();
        database_->migrate();
        service_ = std::make_unique<ExchangeService>(*database_, config_.market.initial_balance_cents,
                                                     config_.market.max_order_quantity);
        hologram_snapshot_state_ = std::make_shared<HologramSnapshotState>();

        // Bedrock marks interactions with an item that has no vanilla action as cancelled before
        // plugins see them. The exchanger is intentionally such an item, so these handlers must
        // still receive cancelled events and decide whether to consume them themselves.
        registerEvent(&ExchangePlugin::onPlayerInteract, *this, endstone::EventPriority::High, false);
        registerEvent(&ExchangePlugin::onPlayerInteractActor, *this, endstone::EventPriority::High, false);
        registerEvent(&ExchangePlugin::onBlockBreak, *this, endstone::EventPriority::Monitor, true);
        registerEvent(&ExchangePlugin::onActorDamage, *this, endstone::EventPriority::Highest, true);
        registerEvent(&ExchangePlugin::onActorRemove, *this, endstone::EventPriority::Monitor);
        registerEvent(&ExchangePlugin::onPlayerJoin, *this, endstone::EventPriority::Monitor);
        registerEvent(&ExchangePlugin::onPlayerQuit, *this, endstone::EventPriority::Monitor);
        registerEvent(&ExchangePlugin::onPlayerMove, *this, endstone::EventPriority::Monitor, true);
        registerEvent(&ExchangePlugin::onChunkLoad, *this, endstone::EventPriority::Monitor);
        registerEvent(&ExchangePlugin::onChunkUnload, *this, endstone::EventPriority::Monitor);
        registerEvent(&ExchangePlugin::onPlayerDropItem, *this, endstone::EventPriority::Highest, true);

        ready_ = true;
        restoreMarkets();
        refresh_task_ = getServer().getScheduler().runTaskTimer(
            *this, [this] { refreshHolograms(); }, 1, config_.market.hologram_refresh_ticks);
        getLogger().info("Endstone Exchange {} enabled with {} active markets.", ENDSTONE_EXCHANGE_VERSION,
                         markets_.size());
    } catch (const std::exception &error) {
        ready_ = false;
        getLogger().error("Exchange startup failed: {}", error.what());
    }
}

void ExchangePlugin::onDisable() {
    ready_ = false;
    if (hologram_snapshot_state_) {
        hologram_snapshot_state_->stopping = true;
        if (hologram_snapshot_state_->query_thread.joinable()) {
            hologram_snapshot_state_->query_thread.request_stop();
            hologram_snapshot_state_->query_thread.join();
        }
    }
    if (refresh_task_) {
        refresh_task_->cancel();
        refresh_task_.reset();
    }
    if (config_.market.cleanup_structure_captures) {
        for (const auto &[key, structure_name] : pending_frame_captures_) {
            static_cast<void>(key);
            static_cast<void>(getServer().dispatchCommand(getServer().getCommandSender(),
                                                          std::format("structure delete {}", structure_name)));
        }
    }
    pending_frame_captures_.clear();
    removeAllHolograms();
    service_.reset();
    database_.reset();
    markets_.clear();
    target_index_.clear();
    hologram_books_.clear();
    hologram_anchors_.clear();
    loaded_chunks_.clear();
    hologram_spawn_ready_chunks_.clear();
    interaction_gate_.clear();
    open_trade_forms_.clear();
    pending_join_hologram_recreates_.clear();
    join_loaded_chunks_.clear();
    pending_hologram_recreates_.clear();
    hologram_snapshot_state_.reset();
    getLogger().info("Endstone Exchange disabled.");
}

bool ExchangePlugin::onCommand(endstone::CommandSender &sender, const endstone::Command &command,
                               const std::vector<std::string> &args) {
    if (command.getName() != "exchange") {
        return false;
    }
    const auto language = languageFor(sender);
    if (!ready_) {
        sender.sendErrorMessage("{}", messageText(language, Message::PluginNotReady));
        return true;
    }

    try {
        auto *player = sender.asPlayer();
        if (args.empty()) {
            if (player != nullptr) {
                service_->ensureAccount(player->getUniqueId().str(), player->getName());
                sender.sendMessage("{}", tr(language, Message::Balance,
                                      formatMoney(service_->balance(player->getUniqueId().str()))));
            }
            sender.sendMessage("{}", messageText(language, Message::CommandHelp));
            return true;
        }

        const auto subcommand = lower(args.front());
        if (subcommand == "give") {
            if (!sender.hasPermission("exchange.admin")) {
                sender.sendErrorMessage("{}", messageText(language, Message::NoGivePermission));
                return true;
            }
            auto *target = player;
            if (args.size() == 2) {
                target = getServer().getPlayer(args[1]);
                if (target == nullptr) {
                    sender.sendErrorMessage("{}", messageText(language, Message::PlayerOffline));
                    return true;
                }
            } else if (args.size() != 1 || target == nullptr) {
                sender.sendErrorMessage("{}", messageText(language, Message::GiveUsage));
                return true;
            }
            if (!canAdmin(*target) && target == player) {
                sender.sendErrorMessage("{}", messageText(language, Message::NoMarketPermission));
                return true;
            }
            giveExchanger(*target);
            if (target != player) {
                sender.sendMessage("{}", tr(language, Message::GaveExchanger, target->getName()));
            }
            return true;
        }
        if (subcommand == "balance") {
            if (player == nullptr) {
                sender.sendErrorMessage("{}", messageText(language, Message::ConsoleHint));
                return true;
            }
            service_->ensureAccount(player->getUniqueId().str(), player->getName());
            sender.sendMessage("{}", tr(language, Message::Balance,
                                  formatMoney(service_->balance(player->getUniqueId().str()))));
            return true;
        }
        if (subcommand == "orders") {
            if (player == nullptr) {
                sender.sendErrorMessage("{}", messageText(language, Message::PlayerFormOnly));
                return true;
            }
            openOrdersForm(*player);
            return true;
        }
        if (subcommand == "claim") {
            if (player == nullptr) {
                sender.sendErrorMessage("{}", messageText(language, Message::PlayerClaimOnly));
                return true;
            }
            static_cast<void>(claimDeliveries(*player));
            queueInventoryResync(*player);
            return true;
        }
        if (subcommand == "addbalance") {
            if (!sender.hasPermission("exchange.admin") || args.size() != 3) {
                sender.sendErrorMessage("{}", messageText(language, Message::AddBalanceUsage));
                return true;
            }
            auto *target = getServer().getPlayer(args[1]);
            if (target == nullptr) {
                sender.sendErrorMessage("{}", messageText(language, Message::PlayerOffline));
                return true;
            }
            std::size_t consumed = 0;
            double amount = 0.0;
            try {
                amount = std::stod(args[2], &consumed);
            } catch (const std::exception &) {
                throw UserError(tr(language, Message::InvalidAmount));
            }
            if (consumed != args[2].size() || !std::isfinite(amount)) {
                throw UserError(tr(language, Message::InvalidAmount));
            }
            const auto raw_cents = static_cast<long double>(amount) * 100.0L;
            if (raw_cents < static_cast<long double>(std::numeric_limits<Cents>::min()) ||
                raw_cents > static_cast<long double>(std::numeric_limits<Cents>::max())) {
                throw UserError(tr(language, Message::AmountOutOfRange));
            }
            const auto cents = static_cast<Cents>(std::llround(raw_cents));
            const auto balance_cents = service_->addBalance(target->getUniqueId().str(), target->getName(), cents);
            sender.sendMessage("{}", tr(language, Message::PlayerBalanceNow, target->getName(),
                                         formatMoney(balance_cents)));
            target->sendMessage("{}", tr(languageFor(*target), Message::BalanceAdjusted, formatMoney(balance_cents)));
            return true;
        }
        if (subcommand == "status") {
            database_->ping();
            sender.sendMessage("{}", tr(language, Message::StatusOk, ENDSTONE_EXCHANGE_VERSION, markets_.size()));
            return true;
        }
        sender.sendErrorMessage("{}", messageText(language, Message::UnknownSubcommand));
    } catch (const std::exception &error) {
        sender.sendErrorMessage("{}", tr(language, Message::CommandFailed, userFacingError(language, error)));
        getLogger().warning("Command failed: {}", error.what());
    }
    return true;
}

void ExchangePlugin::onPlayerInteract(endstone::PlayerInteractEvent &event) {
    if (!ready_ || event.getAction() != endstone::PlayerInteractEvent::Action::RightClickBlock ||
        event.getBlock() == nullptr) {
        return;
    }
    auto &player = event.getPlayer();
    auto &block = *event.getBlock();
    if (event.getItem() && isInternalEscrowItem(*event.getItem())) {
        event.cancel();
        player.sendErrorMessage("{}", messageText(languageFor(player), Message::RecoveryCannotUse));
        return;
    }
    const auto key = blockTargetKey(block);
    const bool exchanger = isExchanger(event.getItem());
    const auto market = target_index_.find(key);
    if (!exchanger && market == target_index_.end()) {
        return;
    }
    if (!acceptInteraction(player)) {
        event.cancel();
        return;
    }
    if (exchanger) {
        event.cancel();
        toggleBlock(player, block);
        return;
    }
    event.cancel();
    queueTradeForm(player, market->second);
}

void ExchangePlugin::onPlayerInteractActor(endstone::PlayerInteractActorEvent &event) {
    if (!ready_) {
        return;
    }
    auto &player = event.getPlayer();
    auto &actor = event.getActor();
    const auto held = player.getInventory().getItemInMainHand();
    if (held && isInternalEscrowItem(*held)) {
        event.cancel();
        player.sendErrorMessage("{}", messageText(languageFor(player), Message::RecoveryCannotUse));
        return;
    }
    const bool exchanger = isExchanger(held);
    const auto market_id = marketIdForActor(actor);
    if (!exchanger && !market_id) {
        return;
    }
    if (!acceptInteraction(player)) {
        event.cancel();
        return;
    }
    if (exchanger) {
        event.cancel();
        if (market_id && hasTag(actor, hologramTag(*market_id))) {
            player.sendErrorMessage("{}", messageText(languageFor(player), Message::HologramNotTarget));
            return;
        }
        toggleActor(player, actor);
        return;
    }
    event.cancel();
    queueTradeForm(player, *market_id);
}

void ExchangePlugin::onBlockBreak(endstone::BlockBreakEvent &event) {
    if (!ready_) {
        return;
    }
    const auto key = blockTargetKey(event.getBlock());
    if (const auto it = target_index_.find(key); it != target_index_.end()) {
        deactivate(&event.getPlayer(), it->second, "target block broken", false);
    }
}

void ExchangePlugin::onActorDamage(endstone::ActorDamageEvent &event) {
    if (!ready_) {
        return;
    }
    for (const auto &tag : event.getActor().getScoreboardTags()) {
        if (tag.starts_with(HologramTagPrefix)) {
            event.cancel();
            return;
        }
    }
    for (const auto &[market_id, actor_id] : hologram_ids_) {
        static_cast<void>(market_id);
        if (event.getActor().getId() == actor_id) {
            event.cancel();
            return;
        }
    }
}

void ExchangePlugin::onActorRemove(endstone::ActorRemoveEvent &event) {
    if (!ready_) {
        return;
    }
    auto &actor = event.getActor();
    std::optional<Id> removed_hologram;
    for (const auto &[market_id, actor_id] : hologram_ids_) {
        if (actor.getId() == actor_id) {
            removed_hologram = market_id;
            break;
        }
    }
    if (removed_hologram) {
        hologram_ids_.erase(*removed_hologram);
        return;
    }
    for (const auto &[market_id, market] : markets_) {
        if (market.target_kind == TargetKind::Actor && hasTag(actor, targetTag(market_id))) {
            deactivate(nullptr, market_id, "target actor removed", false);
            return;
        }
    }
}

void ExchangePlugin::onPlayerJoin(endstone::PlayerJoinEvent &event) {
    if (!ready_) {
        return;
    }
    try {
        auto &player = event.getPlayer();
        open_trade_forms_.erase(player.getUniqueId().str());
        localizeExchangers(player);
        service_->ensureAccount(player.getUniqueId().str(), player.getName());
        static_cast<void>(claimDeliveries(player));
        queueInventoryResync(player);
        pending_join_hologram_recreates_.insert(player.getUniqueId().str());
        join_loaded_chunks_[player.getUniqueId().str()] = loaded_chunks_;
        queuePlayerHologramSync(player, true);
    } catch (const std::exception &error) {
        getLogger().warning("Join settlement failed for {}: {}", event.getPlayer().getName(), error.what());
    }
}

void ExchangePlugin::onPlayerQuit(endstone::PlayerQuitEvent &event) {
    const auto player_uuid = event.getPlayer().getUniqueId().str();
    open_trade_forms_.erase(player_uuid);
    pending_join_hologram_recreates_.erase(player_uuid);
    join_loaded_chunks_.erase(player_uuid);
}

void ExchangePlugin::onPlayerMove(endstone::PlayerMoveEvent &event) {
    if (!ready_) {
        return;
    }
    const auto &from = event.getFrom();
    const auto &to = event.getTo();
    if (from.getDimension().getName() == to.getDimension().getName() &&
        blockToChunk(from.getBlockX()) == blockToChunk(to.getBlockX()) &&
        blockToChunk(from.getBlockZ()) == blockToChunk(to.getBlockZ())) {
        return;
    }
    auto &player = event.getPlayer();
    queuePlayerHologramSync(player, pending_join_hologram_recreates_.contains(player.getUniqueId().str()));
}

void ExchangePlugin::onChunkLoad(endstone::ChunkLoadEvent &event) {
    if (!ready_) {
        return;
    }
    auto &chunk = event.getChunk();
    const auto dimension_name = chunk.getDimension().getName();
    const auto key = chunkKey(dimension_name, chunk.getX(), chunk.getZ());
    loaded_chunks_.insert(key);
    hologram_spawn_ready_chunks_.erase(key);
    queueLoadedChunkReconcile(dimension_name, chunk.getX(), chunk.getZ());
}

void ExchangePlugin::onChunkUnload(endstone::ChunkUnloadEvent &event) {
    auto &chunk = event.getChunk();
    const auto dimension_name = chunk.getDimension().getName();
    std::vector<Id> unloading_holograms;
    for (const auto &[market_id, market] : markets_) {
        if (marketTargetsChunk(market, dimension_name, chunk.getX(), chunk.getZ())) {
            unloading_holograms.push_back(market_id);
        }
    }
    // Holograms are runtime presentation state, not world data. Removing them before the
    // chunk is saved prevents old armor stands from being persisted and replayed alongside
    // the fresh label that will be created after the next ChunkLoadEvent grace period.
    for (const auto market_id : unloading_holograms) {
        pending_hologram_recreates_.erase(market_id);
        removeHologram(market_id);
    }

    const auto key = chunkKey(dimension_name, chunk.getX(), chunk.getZ());
    loaded_chunks_.erase(key);
    hologram_spawn_ready_chunks_.erase(key);
}

void ExchangePlugin::onPlayerDropItem(endstone::PlayerDropItemEvent &event) {
    if (isInternalEscrowItem(event.getItem())) {
        event.setCancelled(true);
        auto &player = event.getPlayer();
        player.sendErrorMessage("{}", messageText(languageFor(player), Message::RecoveryCannotDrop));
    }
}

bool ExchangePlugin::isExchanger(const std::optional<endstone::ItemStack> &item) const {
    if (!item || std::string(item->getType().getId()) != "minecraft:stick") {
        return false;
    }
    const auto meta = item->getItemMeta();
    return meta && meta->hasDisplayName() && lower(stripFormatting(meta->getDisplayName())) == "exchanger";
}

bool ExchangePlugin::canAdmin(endstone::Player &player) const {
    return !config_.market.admin_only || player.hasPermission("exchange.admin");
}

std::string ExchangePlugin::blockTargetKey(const endstone::Block &block) const {
    return std::format("block|{}|{}|{}|{}", block.getDimension().getName(), block.getX(), block.getY(), block.getZ());
}

std::string ExchangePlugin::actorTargetKey(const endstone::Actor &actor) const {
    return std::format("actor|{}|{}", actor.getDimension().getName(), actor.getId());
}

std::optional<Id> ExchangePlugin::marketIdForActor(const endstone::Actor &actor) const {
    for (const auto &[market_id, market] : markets_) {
        if (market.target_kind == TargetKind::Actor && hasTag(actor, targetTag(market_id))) {
            return market_id;
        }
        if (hasTag(actor, hologramTag(market_id))) {
            return market_id;
        }
    }
    return std::nullopt;
}

std::optional<ItemPrototype> ExchangePlugin::prototypeForBlock(endstone::Block &block) {
    const auto block_type = block.getType();
    std::vector<std::string> candidates{block_type};
    if (block_type == "minecraft:frame") {
        candidates.push_back("minecraft:item_frame");
    } else if (block_type == "minecraft:glow_frame") {
        candidates.push_back("minecraft:glow_item_frame");
    }
    for (const auto &candidate : candidates) {
        if (const auto *type = endstone::ItemType::get(endstone::ItemTypeId(candidate)); type != nullptr) {
            const auto item = type->createItemStack(1);
            return ItemPrototype{candidate, item.getData(), NbtCodec::encode(item.getNbt()), friendlyItemName(item)};
        }
    }
    return std::nullopt;
}

std::optional<ItemPrototype> ExchangePlugin::prototypeForActor(endstone::Actor &actor) {
    if (const auto *item_actor = actor.asItem(); item_actor != nullptr) {
        const auto item = item_actor->getItemStack();
        return ItemPrototype{std::string(item.getType().getId()), item.getData(), NbtCodec::encode(item.getNbt()),
                             friendlyItemName(item)};
    }
    const auto actor_type = actor.getType();
    std::vector<std::string> candidates{actor_type};
    const auto colon = actor_type.find(':');
    const auto key = colon == std::string::npos ? actor_type : actor_type.substr(colon + 1);
    candidates.push_back("minecraft:" + key + "_spawn_egg");
    for (const auto &candidate : candidates) {
        if (const auto *type = endstone::ItemType::get(endstone::ItemTypeId(candidate)); type != nullptr) {
            const auto item = type->createItemStack(1);
            return ItemPrototype{candidate, item.getData(), NbtCodec::encode(item.getNbt()), friendlyItemName(item)};
        }
    }
    return std::nullopt;
}

bool ExchangePlugin::acceptInteraction(const endstone::Player &player) {
    return interaction_gate_.accept(player.getUniqueId().str());
}

void ExchangePlugin::toggleBlock(endstone::Player &player, endstone::Block &block) {
    const auto language = languageFor(player);
    if (!canAdmin(player)) {
        player.sendErrorMessage("{}", messageText(language, Message::NoMarketPermission));
        return;
    }
    const auto key = blockTargetKey(block);
    if (const auto it = target_index_.find(key); it != target_index_.end()) {
        deactivate(&player, it->second, "closed by administrator", true);
        return;
    }
    if (isItemFrameBlock(block.getType())) {
        queueItemFrameToggle(player, block);
        return;
    }
    try {
        const auto prototype = prototypeForBlock(block);
        if (!prototype) {
            player.sendErrorMessage("{}", messageText(language, Message::BlockNoItem));
            return;
        }
        Market market;
        market.target_key = key;
        market.target_kind = TargetKind::Block;
        market.dimension_name = block.getDimension().getName();
        market.block_x = block.getX();
        market.block_y = block.getY();
        market.block_z = block.getZ();
        market.item = *prototype;
        market.created_by = player.getUniqueId().str();
        market = service_->activateMarket(market);
        indexMarket(market);
        refreshHologram(market);
        player.sendMessage("{}", tr(language, Message::ObjectEnabled, localizedItemName(market.item, language)));
    } catch (const std::exception &error) {
        player.sendErrorMessage("{}", tr(language, Message::ActivationFailed, userFacingError(language, error)));
        getLogger().warning("Block activation failed: {}", error.what());
    }
}

void ExchangePlugin::queueItemFrameToggle(endstone::Player &player, endstone::Block &block) {
    const auto language = languageFor(player);
    const auto key = blockTargetKey(block);
    if (const auto pending = pending_frame_captures_.find(key); pending != pending_frame_captures_.end()) {
        if (config_.market.cleanup_structure_captures) {
            static_cast<void>(getServer().dispatchCommand(getServer().getCommandSender(),
                                                          std::format("structure delete {}", pending->second)));
        }
        pending_frame_captures_.erase(pending);
        player.sendMessage("{}", messageText(language, Message::FrameCanceled));
        return;
    }

    const auto *level = getServer().getLevel();
    if (level == nullptr) {
        player.sendErrorMessage("{}", messageText(language, Message::WorldNotLoaded));
        return;
    }
    const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto structure_name =
        std::format("exchange:frame_{}_{}_{}_{}", block.getX(), block.getY(), block.getZ(), serial);
    const auto dimension_name = block.getDimension().getName();
    const auto level_name = level->getName();
    const int x = block.getX();
    const int y = block.getY();
    const int z = block.getZ();
    const auto player_uuid = player.getUniqueId().str();
    const auto player_name = player.getName();
    const auto command = std::format("execute in {} run structure save {} {} {} {} {} {} {} false disk true",
                                     dimension_name, structure_name, x, y, z, x, y, z);
    pending_frame_captures_[key] = structure_name;
    if (!getServer().dispatchCommand(getServer().getCommandSender(), command)) {
        pending_frame_captures_.erase(key);
        player.sendErrorMessage("{}", messageText(language, Message::FrameReadFailed));
        return;
    }
    player.sendMessage("{}", messageText(language, Message::FrameReading));

    static_cast<void>(getServer().getScheduler().runTaskLater(
        *this,
        [this, key, structure_name, dimension_name, level_name, x, y, z, player_uuid, player_name] {
            const auto pending = pending_frame_captures_.find(key);
            if (pending == pending_frame_captures_.end() || pending->second != structure_name) {
                return;
            }
            pending_frame_captures_.erase(pending);
            const auto cleanup = [this, &structure_name] {
                if (config_.market.cleanup_structure_captures) {
                    static_cast<void>(getServer().dispatchCommand(getServer().getCommandSender(),
                                                                  std::format("structure delete {}", structure_name)));
                }
            };
            auto *notify = getServer().getPlayer(player_name);
            if (notify != nullptr && notify->getUniqueId().str() != player_uuid) {
                notify = nullptr;
            }
            const auto notify_language =
                notify == nullptr ? Language::SimplifiedChinese : languageFor(*notify);
            try {
                if (!ready_) {
                    cleanup();
                    return;
                }
                const auto database_path = std::filesystem::current_path() / "worlds" / level_name / "db";
                const auto captured = StructureReader::readItemFrameFromLevelDbLogs(database_path, structure_name);
                const auto *current_level = getServer().getLevel();
                auto *dimension = current_level == nullptr ? nullptr : current_level->getDimension(dimension_name);
                auto current_block = dimension == nullptr ? nullptr : dimension->getBlockAt(x, y, z);
                if (!current_block || !isItemFrameBlock(current_block->getType())) {
                    throw UserError(tr(notify_language, Message::FrameMissing));
                }
                if (target_index_.contains(key)) {
                    cleanup();
                    return;
                }

                std::optional<ItemPrototype> prototype;
                if (captured) {
                    endstone::ItemStack item(endstone::ItemTypeId(captured->type), 1, captured->data);
                    item.setNbt(captured->nbt);
                    prototype = ItemPrototype{captured->type, captured->data, NbtCodec::encode(captured->nbt),
                                              friendlyItemName(item)};
                } else {
                    prototype = prototypeForBlock(*current_block);
                }
                if (!prototype) {
                    throw UserError(tr(notify_language, Message::FrameNoItem));
                }

                Market market;
                market.target_key = key;
                market.target_kind = TargetKind::Block;
                market.dimension_name = dimension_name;
                market.block_x = x;
                market.block_y = y;
                market.block_z = z;
                market.item = *prototype;
                market.created_by = player_uuid;
                market = service_->activateMarket(market);
                indexMarket(market);
                refreshHologram(market);
                if (notify != nullptr) {
                    notify->sendMessage("{}", tr(notify_language, Message::FrameEnabled,
                                                 localizedItemName(market.item, notify_language)));
                }
                cleanup();
            } catch (const std::exception &error) {
                cleanup();
                if (notify != nullptr) {
                    notify->sendErrorMessage("{}", tr(notify_language, Message::FrameActivationFailed,
                                                      userFacingError(notify_language, error)));
                }
                getLogger().warning("Item-frame activation failed: {}", error.what());
            }
        },
        config_.market.frame_capture_delay_ticks));
}

void ExchangePlugin::toggleActor(endstone::Player &player, endstone::Actor &actor) {
    const auto language = languageFor(player);
    if (!canAdmin(player)) {
        player.sendErrorMessage("{}", messageText(language, Message::NoMarketPermission));
        return;
    }
    if (const auto market_id = marketIdForActor(actor)) {
        deactivate(&player, *market_id, "closed by administrator", true);
        return;
    }
    try {
        const auto prototype = prototypeForActor(actor);
        if (!prototype) {
            player.sendErrorMessage("{}", messageText(language, Message::ActorNoItem));
            return;
        }
        Market market;
        market.target_key = actorTargetKey(actor);
        market.target_kind = TargetKind::Actor;
        market.dimension_name = actor.getDimension().getName();
        market.actor_id = actor.getId();
        market.item = *prototype;
        market.created_by = player.getUniqueId().str();
        market = service_->activateMarket(market);
        static_cast<void>(actor.addScoreboardTag(targetTag(market.id)));
        indexMarket(market);
        refreshHologram(market);
        player.sendMessage("{}", tr(language, Message::ActorEnabled, localizedItemName(market.item, language)));
    } catch (const std::exception &error) {
        player.sendErrorMessage("{}", tr(language, Message::ActivationFailed, userFacingError(language, error)));
        getLogger().warning("Actor activation failed: {}", error.what());
    }
}

void ExchangePlugin::deactivate(endstone::Player *player, const Id market_id, const std::string_view reason,
                                const bool preserve_orders) {
    try {
        const auto it = markets_.find(market_id);
        if (it == markets_.end()) {
            return;
        }
        if (it->second.target_kind == TargetKind::Actor) {
            if (auto *target = findActorByTag(targetTag(market_id)); target != nullptr) {
                static_cast<void>(target->removeScoreboardTag(targetTag(market_id)));
            }
        }
        if (preserve_orders) {
            service_->closeMarket(market_id);
        } else {
            service_->retireMarket(market_id);
        }
        removeHologram(market_id);
        unindexMarket(market_id);
        if (player != nullptr) {
            const auto language = languageFor(*player);
            if (preserve_orders) {
                player->sendMessage("{}", messageText(language, Message::MarketClosedPreserved));
            } else {
                player->sendMessage("{}", messageText(language, Message::MarketRemoved));
            }
        }
        getLogger().info("Market {} deactivated: {} (orders_preserved={})", market_id, reason, preserve_orders);
    } catch (const std::exception &error) {
        if (player != nullptr) {
            const auto language = languageFor(*player);
            player->sendErrorMessage("{}",
                                     tr(language, Message::MarketCloseFailed, userFacingError(language, error)));
        }
        getLogger().warning("Market {} deactivation failed: {}", market_id, error.what());
    }
}

void ExchangePlugin::indexMarket(const Market &market) {
    markets_[market.id] = market;
    target_index_[market.target_key] = market.id;
}

void ExchangePlugin::unindexMarket(const Id market_id) {
    if (const auto it = markets_.find(market_id); it != markets_.end()) {
        target_index_.erase(it->second.target_key);
        markets_.erase(it);
    }
}

void ExchangePlugin::restoreMarkets() {
    markets_.clear();
    target_index_.clear();
    hologram_ids_.clear();
    hologram_books_.clear();
    hologram_anchors_.clear();
    loaded_chunks_.clear();
    hologram_spawn_ready_chunks_.clear();
    pending_hologram_recreates_.clear();
    // Keep the snapshot state created by onEnable(). Resetting it here disables both the
    // initial asynchronous order-book query and every periodic hologram refresh afterwards.
    if (const auto *level = getServer().getLevel(); level != nullptr) {
        for (auto *dimension : level->getDimensions()) {
            if (dimension == nullptr) {
                continue;
            }
            for (auto &chunk : dimension->getLoadedChunks()) {
                if (chunk != nullptr) {
                    loaded_chunks_.insert(chunkKey(dimension->getName(), chunk->getX(), chunk->getZ()));
                    queueLoadedChunkReconcile(dimension->getName(), chunk->getX(), chunk->getZ());
                }
            }
        }
        for (auto *actor : level->getActors()) {
            if (actor == nullptr) {
                continue;
            }
            for (const auto &tag : actor->getScoreboardTags()) {
                if (tag.starts_with(HologramTagPrefix)) {
                    actor->remove();
                    break;
                }
            }
        }
    }
    for (const auto &market : service_->activeMarkets()) {
        indexMarket(market);
    }
    refreshHolograms();
}

void ExchangePlugin::queueTradeForm(endstone::Player &player, const Id market_id) {
    const auto player_uuid = player.getUniqueId().str();
    const auto player_name = player.getName();
    static_cast<void>(getServer().getScheduler().runTaskLater(
        *this,
        [this, player_uuid, player_name, market_id] {
            if (!ready_) {
                return;
            }
            auto *current = getServer().getPlayer(player_name);
            if (current != nullptr && current->getUniqueId().str() == player_uuid) {
                openTradeForm(*current, market_id);
            }
        },
        1));
}

void ExchangePlugin::queueTradeInputForm(endstone::Player &player, const Id market_id, const TradeDraft draft) {
    const auto player_uuid = player.getUniqueId().str();
    const auto player_name = player.getName();
    static_cast<void>(getServer().getScheduler().runTaskLater(
        *this,
        [this, player_uuid, player_name, market_id, draft] {
            if (!ready_) {
                return;
            }
            auto *current = getServer().getPlayer(player_name);
            if (current != nullptr && current->getUniqueId().str() == player_uuid) {
                openTradeInputForm(*current, market_id, draft);
            }
        },
        1));
}

void ExchangePlugin::queueTradeReviewForm(endstone::Player &player, const Id market_id, const TradeDraft draft) {
    const auto player_uuid = player.getUniqueId().str();
    const auto player_name = player.getName();
    static_cast<void>(getServer().getScheduler().runTaskLater(
        *this,
        [this, player_uuid, player_name, market_id, draft] {
            if (!ready_) {
                return;
            }
            auto *current = getServer().getPlayer(player_name);
            if (current != nullptr && current->getUniqueId().str() == player_uuid) {
                openTradeReviewForm(*current, market_id, draft);
            }
        },
        1));
}

void ExchangePlugin::queueTradeSubmission(endstone::Player &player, const Id market_id, const TradeDraft draft) {
    const auto player_uuid = player.getUniqueId().str();
    const auto player_name = player.getName();
    static_cast<void>(getServer().getScheduler().runTaskLater(
        *this,
        [this, player_uuid, player_name, market_id, draft] {
            if (!ready_) {
                return;
            }
            auto *current = getServer().getPlayer(player_name);
            if (current != nullptr && current->getUniqueId().str() == player_uuid) {
                submitTradeDraft(*current, market_id, draft);
            }
        },
        1));
}

void ExchangePlugin::openTradeForm(endstone::Player &player, const Id market_id) {
    const auto player_uuid = player.getUniqueId().str();
    const auto language = languageFor(player);
    if (!open_trade_forms_.insert(player_uuid).second) {
        return;
    }
    try {
        const auto market_it = markets_.find(market_id);
        if (market_it == markets_.end()) {
            open_trade_forms_.erase(player_uuid);
            player.sendErrorMessage("{}", messageText(language, Message::TradePointClosed));
            return;
        }
        service_->ensureAccount(player.getUniqueId().str(), player.getName());
        static_cast<void>(claimDeliveries(player));
        queueInventoryResync(player);
        const auto book = service_->orderBook(market_id, config_.market.order_book_depth);
        const auto account_balance = service_->balance(player.getUniqueId().str());
        const auto sellable_quantity = sellableItemCount(player.getInventory(), market_it->second.item);

        Cents reference = book.last_price_cents.value_or(1000);
        if (!book.bids.empty() && !book.asks.empty()) {
            reference = std::midpoint(book.bids.front().price_cents, book.asks.front().price_cents);
        } else if (!book.bids.empty()) {
            reference = book.bids.front().price_cents;
        } else if (!book.asks.empty()) {
            reference = book.asks.front().price_cents;
        }
        const auto default_price = defaultTradePrice(config_.market.price_min_cents, config_.market.price_max_cents,
                                                     config_.market.price_step_cents, reference);
        const auto item_name = localizedItemName(market_it->second.item, language);
        const auto item_requirements = describeItemRequirements(market_it->second.item, language, item_name);

        endstone::ActionForm form;
        form.setTitle(tr(language, Message::TradeTitle, item_name))
            .setContent(tradeOverviewText(book, formatCurrency(account_balance), sellable_quantity,
                                          item_requirements, language));

        const auto actions = availableTradeActions(!book.bids.empty(), !book.asks.empty());
        for (const auto action : actions) {
            form.addButton(std::string(tradeActionLabel(action, language)), std::nullopt,
                           [this, market_id, action, default_price, player_uuid](endstone::Player *form_player) {
                open_trade_forms_.erase(player_uuid);
                if (form_player == nullptr || !ready_ || form_player->getUniqueId().str() != player_uuid) {
                    return;
                }
                form_player->closeForm();
                const TradeDraft draft{action, 1, tradeActionUsesPrice(action) ? default_price : 0};
                queueTradeInputForm(*form_player, market_id, draft);
            });
        }
        form.setOnClose([this, player_uuid](endstone::Player *) { open_trade_forms_.erase(player_uuid); });
        player.sendForm(std::move(form));
    } catch (const std::exception &error) {
        open_trade_forms_.erase(player_uuid);
        player.sendErrorMessage("{}", tr(language, Message::OpenTradeFailed, userFacingError(language, error)));
        getLogger().warning("Open form failed: {}", error.what());
    }
}

void ExchangePlugin::openTradeInputForm(endstone::Player &player, const Id market_id, const TradeDraft draft) {
    const auto player_uuid = player.getUniqueId().str();
    const auto language = languageFor(player);
    if (!open_trade_forms_.insert(player_uuid).second) {
        return;
    }
    try {
        const auto market_it = markets_.find(market_id);
        if (market_it == markets_.end()) {
            open_trade_forms_.erase(player_uuid);
            player.sendErrorMessage("{}", messageText(language, Message::TradePointClosed));
            return;
        }
        const auto book = service_->orderBook(market_id, 1);
        const auto actions = availableTradeActions(!book.bids.empty(), !book.asks.empty());
        if (std::find(actions.begin(), actions.end(), draft.action) == actions.end()) {
            open_trade_forms_.erase(player_uuid);
            player.sendErrorMessage("{}", messageText(language, Message::MatchingOrderGone));
            queueTradeForm(player, market_id);
            return;
        }

        endstone::ModalForm form;
        const auto item_name = localizedItemName(market_it->second.item, language);
        form.setTitle(std::string(tradeActionLabel(draft.action, language)) + " · " + item_name)
            .addControl(endstone::TextInput(
                tr(language, Message::QuantityInput, config_.market.max_order_quantity),
                std::string(messageText(language, Message::IntegerPlaceholder)),
                std::to_string(draft.quantity)));
        if (tradeActionUsesPrice(draft.action)) {
            form.addControl(endstone::TextInput(
                tr(language, Message::UnitPriceInput, formatUnitPrice(config_.market.price_min_cents),
                   formatUnitPrice(config_.market.price_max_cents)),
                std::string(messageText(language, Message::PricePlaceholder)),
                std::to_string(draft.price_cents / 100)));
        }
        form.setSubmitButton(std::string(messageText(language, Message::ContinueButton)))
            .setOnClose([this, player_uuid](endstone::Player *) { open_trade_forms_.erase(player_uuid); })
            .setOnSubmit([this, market_id, draft, player_uuid](endstone::Player *form_player, std::string response) {
                open_trade_forms_.erase(player_uuid);
                if (form_player == nullptr || !ready_ || form_player->getUniqueId().str() != player_uuid) {
                    return;
                }
                form_player->closeForm();
                try {
                    const auto form_language = languageFor(*form_player);
                    const auto values = textFormValues(response, form_language);
                    const auto expected_values = tradeActionUsesPrice(draft.action) ? 2U : 1U;
                    if (values.size() != expected_values) {
                        throw UserError(tr(form_language, Message::InvalidFormData));
                    }
                    auto updated = draft;
                    updated.quantity =
                        parseTradeQuantity(values[0], config_.market.max_order_quantity, form_language);
                    if (tradeActionUsesPrice(draft.action)) {
                        updated.price_cents =
                            parseTradePrice(values[1], config_.market.price_min_cents,
                                            config_.market.price_max_cents, config_.market.price_step_cents,
                                            form_language);
                    }
                    queueTradeReviewForm(*form_player, market_id, updated);
                } catch (const std::exception &error) {
                    const auto form_language = languageFor(*form_player);
                    form_player->sendErrorMessage(
                        "{}", tr(form_language, Message::InputInvalid, userFacingError(form_language, error)));
                    queueTradeInputForm(*form_player, market_id, draft);
                }
            });
        player.sendForm(std::move(form));
    } catch (const std::exception &error) {
        open_trade_forms_.erase(player_uuid);
        player.sendErrorMessage("{}", tr(language, Message::OpenInputFailed, userFacingError(language, error)));
        getLogger().warning("Open trade input form failed: {}", error.what());
    }
}

void ExchangePlugin::openTradeReviewForm(endstone::Player &player, const Id market_id, const TradeDraft draft) {
    const auto player_uuid = player.getUniqueId().str();
    const auto language = languageFor(player);
    if (!open_trade_forms_.insert(player_uuid).second) {
        return;
    }
    try {
        const auto market_it = markets_.find(market_id);
        if (market_it == markets_.end()) {
            open_trade_forms_.erase(player_uuid);
            player.sendErrorMessage("{}", messageText(language, Message::TradePointClosed));
            return;
        }
        const auto book = service_->orderBook(market_id, 1);
        const auto actions = availableTradeActions(!book.bids.empty(), !book.asks.empty());
        if (std::find(actions.begin(), actions.end(), draft.action) == actions.end()) {
            open_trade_forms_.erase(player_uuid);
            player.sendErrorMessage("{}", messageText(language, Message::MatchingOrderGone));
            queueTradeForm(player, market_id);
            return;
        }

        endstone::MessageForm form;
        const auto item_name = localizedItemName(market_it->second.item, language);
        form.setTitle(tr(language, Message::ConfirmTitle, item_name))
            .setContent(tradeReviewText(draft, language))
            .setButton1(std::string(messageText(language, Message::ConfirmButton)))
            .setButton2(std::string(messageText(language, Message::EditTradeButton)))
            .setOnSubmit([this, market_id, draft, player_uuid](endstone::Player *form_player, const int selection) {
                open_trade_forms_.erase(player_uuid);
                if (form_player == nullptr || !ready_ || form_player->getUniqueId().str() != player_uuid) {
                    return;
                }
                form_player->closeForm();
                if (selection == 0) {
                    queueTradeSubmission(*form_player, market_id, draft);
                } else {
                    queueTradeInputForm(*form_player, market_id, draft);
                }
            })
            .setOnClose([this, player_uuid](endstone::Player *) { open_trade_forms_.erase(player_uuid); });
        player.sendForm(std::move(form));
    } catch (const std::exception &error) {
        open_trade_forms_.erase(player_uuid);
        player.sendErrorMessage("{}", tr(language, Message::OpenReviewFailed, userFacingError(language, error)));
        getLogger().warning("Open trade review form failed: {}", error.what());
    }
}

void ExchangePlugin::submitTradeDraft(endstone::Player &player, const Id market_id, const TradeDraft &draft) {
    const auto language = languageFor(player);
    try {
        if (draft.quantity < 1 || draft.quantity > config_.market.max_order_quantity) {
            throw UserError(tr(language, Message::QuantityRangeInvalid));
        }
        if (tradeActionUsesPrice(draft.action) &&
            (draft.price_cents < config_.market.price_min_cents ||
             draft.price_cents > config_.market.price_max_cents ||
             (draft.price_cents - config_.market.price_min_cents) % config_.market.price_step_cents != 0 ||
             draft.price_cents % 100 != 0)) {
            throw UserError(tr(language, Message::PriceRangeInvalid));
        }
        const auto market_it = markets_.find(market_id);
        if (market_it == markets_.end()) {
            throw UserError(tr(language, Message::TradePointClosed));
        }
        if (!tradeActionUsesPrice(draft.action)) {
            const auto book = service_->orderBook(market_id, 1);
            const auto actions = availableTradeActions(!book.bids.empty(), !book.asks.empty());
            if (std::find(actions.begin(), actions.end(), draft.action) == actions.end()) {
                throw UserError(tr(language, Message::MatchingOrderGone));
            }
        }
        OrderRequest request;
        request.market_id = market_id;
        request.player_uuid = player.getUniqueId().str();
        request.player_name = player.getName();
        request.side = tradeActionSide(draft.action);
        request.type = tradeActionOrderType(draft.action);
        request.price_cents = tradeActionUsesPrice(draft.action) ? draft.price_cents : 0;
        request.quantity = draft.quantity;
        const auto result =
            request.side == Side::Sell ? submitSellEscrow(player, request) : service_->placeOrder(request);
        int delivered_items = 0;
        bool delivery_pending = false;
        try {
            delivered_items = claimDeliveries(player, false);
        } catch (const std::exception &delivery_error) {
            delivery_pending = true;
            getLogger().warning("Post-trade delivery remains pending for {} after order {}: {}", player.getName(),
                                result.order_id, delivery_error.what());
        }
        refreshHologram(market_it->second);
        std::string delivery_note;
        if (delivered_items > 0) {
            delivery_note = request.side == Side::Buy ? tr(language, Message::DeliveryReceived, delivered_items)
                                                      : tr(language, Message::DeliveryReturned, delivered_items);
        }
        if (delivery_pending) {
            delivery_note += messageText(language, Message::DeliveryPending);
        }

        if (result.filled_quantity == 0 && result.open_quantity == 0) {
            const auto unavailable = messageText(
                language, request.side == Side::Buy ? Message::OtherPlayerSelling : Message::OtherPlayerBuying);
            player.sendMessage("{}", tr(language, Message::OrderNoFill, result.order_id, unavailable, delivery_note,
                                        formatMoney(result.balance_cents)));
        } else if (result.filled_quantity == 0) {
            player.sendMessage(
                "{}", tr(language, Message::OrderSaved, result.order_id, formatUnitPrice(request.price_cents),
                         messageText(language, request.side == Side::Buy ? Message::BuyVerb : Message::SellVerb),
                         result.open_quantity, delivery_note, formatMoney(result.balance_cents)));
        } else if (result.open_quantity > 0) {
            player.sendMessage("{}", tr(language, Message::OrderPartiallyFilled, result.order_id,
                                        result.filled_quantity, result.open_quantity,
                                        formatMoney(result.gross_cents), delivery_note,
                                        formatMoney(result.balance_cents)));
        } else if (result.filled_quantity == result.requested_quantity) {
            player.sendMessage("{}", tr(language, Message::TradeCompleted, result.order_id,
                                        result.filled_quantity, formatMoney(result.gross_cents), delivery_note,
                                        formatMoney(result.balance_cents)));
        } else {
            player.sendMessage("{}", tr(language, Message::OrderEnded, result.order_id, result.filled_quantity,
                                        result.requested_quantity, formatMoney(result.gross_cents), delivery_note,
                                        formatMoney(result.balance_cents)));
        }
        queueInventoryResync(player);
    } catch (const std::exception &error) {
        player.sendErrorMessage("{}", tr(language, Message::TradeFailed, userFacingError(language, error)));
        getLogger().warning("Trade submission failed for {}: {}", player.getName(), error.what());
        queueInventoryResync(player);
    }
}

ExecutionResult ExchangePlugin::submitSellEscrow(endstone::Player &player, const OrderRequest &request) {
    auto &inventory = player.getInventory();
    const auto language = languageFor(player);
    const auto escrow = service_->prepareSellEscrow(request);
    try {
        const auto item_name = localizedItemName(escrow.item, language);
        const auto tagged = tagSellItems(inventory, escrow.item, escrow.requested_quantity, escrow.id, language,
                                         item_name);
        const auto persisted_tagged = taggedSellItems(inventory, escrow.id);
        if (persisted_tagged.quantity != tagged.quantity || persisted_tagged.stacks != tagged.stacks) {
            throw std::runtime_error("sell items were not fully tagged for escrow");
        }
        service_->markSellEscrowTagged(escrow.id, tagged.quantity, tagged.stacks);
        replaceTaggedSellItemsWithReceipts(inventory, escrow.id, language);
        if (sellReceiptCount(inventory, escrow.id) != tagged.stacks) {
            throw std::runtime_error("sell escrow receipt count does not match tagged stacks");
        }
        auto result = service_->executeSellEscrow(escrow.id);
        removeTaggedSellItems(inventory, escrow.id);
        removeSellReceipts(inventory, escrow.id);
        if (taggedSellItems(inventory, escrow.id).quantity != 0 || sellReceiptCount(inventory, escrow.id) != 0) {
            throw std::runtime_error("sell escrow markers were not fully cleared");
        }
        service_->markSellEscrowCleaned(escrow.id);
        return result;
    } catch (const std::exception &error) {
        const std::string original_error = error.what();
        const bool player_facing = dynamic_cast<const UserError *>(&error) != nullptr;
        try {
            reconcileSellEscrows(player);
            return service_->executeSellEscrow(escrow.id);
        } catch (const std::exception &recovery_error) {
            getLogger().warning("Sell escrow {} remains recoverable for {}: {}", escrow.id, player.getName(),
                                recovery_error.what());
        }
        if (player_facing) {
            throw UserError(original_error);
        }
        throw std::runtime_error(original_error);
    }
}

void ExchangePlugin::reconcileInternalEscrowMarkers(endstone::Player &player) {
    auto &inventory = player.getInventory();
    const auto player_uuid = player.getUniqueId().str();
    const auto markers = internalEscrowMarkers(inventory);

    for (const auto claim_id : markers.delivery_claim_ids) {
        const auto claim = service_->findDeliveryClaim(claim_id, player_uuid);
        if (!claim) {
            getLogger().warning("Player {} has an unknown delivery marker {}.", player.getName(), claim_id);
            continue;
        }
        if (claim->status == DeliveryClaimStatus::Applied) {
            clearDeliveryClaimTag(inventory, claim_id, claim->item);
            if (taggedItemCount(inventory, claim_id) != 0) {
                throw std::runtime_error(std::format("delivery marker {} was not cleared from inventory", claim_id));
            }
            if (!claim->cleaned) {
                service_->markDeliveryClaimCleaned(claim_id);
            }
        } else if (claim->status == DeliveryClaimStatus::Canceled) {
            removeDeliveryClaimItems(inventory, claim_id);
            if (taggedItemCount(inventory, claim_id) != 0) {
                throw std::runtime_error(std::format("canceled delivery marker {} was not cleared", claim_id));
            }
        }
    }

    std::set<Id> sell_escrow_ids(markers.sell_item_escrow_ids.begin(), markers.sell_item_escrow_ids.end());
    sell_escrow_ids.insert(markers.sell_receipt_escrow_ids.begin(), markers.sell_receipt_escrow_ids.end());
    for (const auto escrow_id : sell_escrow_ids) {
        const auto escrow = service_->findSellEscrow(escrow_id, player_uuid);
        if (!escrow) {
            getLogger().warning("Player {} has an unknown sell marker {}.", player.getName(), escrow_id);
            continue;
        }
        if (escrow->status == SellEscrowStatus::Ordered) {
            removeTaggedSellItems(inventory, escrow_id);
            removeSellReceipts(inventory, escrow_id);
            if (taggedSellItems(inventory, escrow_id).quantity != 0 || sellReceiptCount(inventory, escrow_id) != 0) {
                throw std::runtime_error(std::format("sell escrow marker {} was not cleared", escrow_id));
            }
            if (!escrow->cleaned) {
                service_->markSellEscrowCleaned(escrow_id);
            }
        } else if (escrow->status == SellEscrowStatus::Canceled) {
            restoreTaggedSellItems(inventory, escrow_id, escrow->item);
            removeSellReceipts(inventory, escrow_id);
            if (taggedSellItems(inventory, escrow_id).quantity != 0 || sellReceiptCount(inventory, escrow_id) != 0) {
                throw std::runtime_error(std::format("canceled sell escrow marker {} was not cleared", escrow_id));
            }
        }
    }
}

void ExchangePlugin::reconcileSellEscrows(endstone::Player &player) {
    auto &inventory = player.getInventory();
    const auto language = languageFor(player);
    for (auto escrow : service_->unfinishedSellEscrows(player.getUniqueId().str())) {
        if (escrow.status == SellEscrowStatus::Prepared) {
            const auto tagged = taggedSellItems(inventory, escrow.id);
            if (tagged.quantity < escrow.requested_quantity) {
                if (tagged.quantity > 0) {
                    restoreTaggedSellItems(inventory, escrow.id, escrow.item);
                }
                service_->cancelSellEscrow(escrow.id);
                continue;
            }
            service_->markSellEscrowTagged(escrow.id, tagged.quantity, tagged.stacks);
            escrow.tagged_quantity = tagged.quantity;
            escrow.receipt_count = tagged.stacks;
            escrow.status = SellEscrowStatus::Tagged;
        }
        if (escrow.status == SellEscrowStatus::Tagged) {
            replaceTaggedSellItemsWithReceipts(inventory, escrow.id, language);
            if (sellReceiptCount(inventory, escrow.id) != escrow.receipt_count) {
                throw std::runtime_error("sell escrow receipts were not fully restored");
            }
            static_cast<void>(service_->executeSellEscrow(escrow.id));
            escrow.status = SellEscrowStatus::Ordered;
        }
        if (escrow.status == SellEscrowStatus::Ordered) {
            removeTaggedSellItems(inventory, escrow.id);
            removeSellReceipts(inventory, escrow.id);
            if (taggedSellItems(inventory, escrow.id).quantity != 0 || sellReceiptCount(inventory, escrow.id) != 0) {
                throw std::runtime_error("sell escrow markers were not fully removed");
            }
            service_->markSellEscrowCleaned(escrow.id);
        }
    }
}

void ExchangePlugin::openOrdersForm(endstone::Player &player) {
    const auto language = languageFor(player);
    try {
        const auto player_uuid = player.getUniqueId().str();
        const auto orders = service_->openOrders(player_uuid);
        endstone::ActionForm form;
        form.setTitle(std::string(messageText(language, Message::OrdersTitle)))
            .addHeader(std::string(messageText(language, Message::OrdersHeader)));
        if (orders.empty()) {
            form.addLabel(std::string(messageText(language, Message::NoOpenOrders)));
        }
        for (const auto &order : orders) {
            const auto text = tr(language, Message::OrderButton, order.id,
                                 messageText(language, order.side == Side::Buy ? Message::BuyColored
                                                                              : Message::SellColored),
                                 localizedItemName(order.item, language), order.remaining_quantity);
            form.addButton(text, std::nullopt, [this, order_id = order.id, player_uuid](endstone::Player *form_player) {
                if (form_player == nullptr || !ready_ || form_player->getUniqueId().str() != player_uuid) {
                    return;
                }
                try {
                    service_->cancelOrder(order_id, player_uuid);
                    static_cast<void>(claimDeliveries(*form_player));
                    queueInventoryResync(*form_player);
                    refreshHolograms();
                    const auto form_language = languageFor(*form_player);
                    form_player->sendMessage("{}", tr(form_language, Message::OrderCanceled, order_id));
                } catch (const std::exception &error) {
                    const auto form_language = languageFor(*form_player);
                    form_player->sendErrorMessage(
                        "{}", tr(form_language, Message::CancelOrderFailed, userFacingError(form_language, error)));
                }
            });
        }
        player.sendForm(std::move(form));
    } catch (const std::exception &error) {
        player.sendErrorMessage("{}", tr(language, Message::ReadOrdersFailed, userFacingError(language, error)));
    }
}

int ExchangePlugin::claimDeliveries(endstone::Player &player, const bool announce) {
    reconcileInternalEscrowMarkers(player);
    reconcileSellEscrows(player);
    const auto player_uuid = player.getUniqueId().str();
    auto &inventory = player.getInventory();
    int delivered_total = 0;
    for (const auto &claim : service_->unfinishedDeliveryClaims(player_uuid)) {
        const auto tagged = taggedItemCount(inventory, claim.id);
        if (claim.status == DeliveryClaimStatus::Prepared) {
            const auto delivered = std::min(tagged, claim.quantity);
            service_->completeDeliveryClaim(claim.id, delivered);
            delivered_total += delivered;
            if (delivered == 0) {
                continue;
            }
        }
        if (tagged > 0) {
            clearDeliveryClaimTag(inventory, claim.id, claim.item);
        }
        if (taggedItemCount(inventory, claim.id) != 0) {
            throw std::runtime_error(std::format("delivery marker {} was not cleared from inventory", claim.id));
        }
        service_->markDeliveryClaimCleaned(claim.id);
    }

    const auto deliveries = service_->pendingDeliveries(player_uuid);
    for (const auto &delivery : deliveries) {
        const int pending = delivery.quantity - delivery.claimed_quantity - delivery.reserved_quantity;
        if (pending <= 0) {
            continue;
        }
        const auto claim = service_->prepareDeliveryClaim(delivery.id, player_uuid, pending);
        auto stacks = splitStacks(delivery.item, claim.quantity, claim.id);
        const int attempted =
            std::accumulate(stacks.begin(), stacks.end(), 0,
                            [](int total, const endstone::ItemStack &item) { return total + item.getAmount(); });
        const auto leftovers = inventory.addItem(std::move(stacks));
        const int delivered = attempted - itemCount(leftovers);
        service_->completeDeliveryClaim(claim.id, delivered);
        if (delivered > 0) {
            clearDeliveryClaimTag(inventory, claim.id, delivery.item);
            if (taggedItemCount(inventory, claim.id) != 0) {
                throw std::runtime_error(std::format("delivery marker {} was not cleared from inventory", claim.id));
            }
            service_->markDeliveryClaimCleaned(claim.id);
            delivered_total += delivered;
        }
        if (!leftovers.empty()) {
            break;
        }
    }
    if (announce && delivered_total > 0) {
        player.sendMessage("{}", tr(languageFor(player), Message::ClaimedItems, delivered_total));
    }
    return delivered_total;
}

void ExchangePlugin::queueInventoryResync(endstone::Player &player) {
    const auto player_uuid = player.getUniqueId().str();
    const auto player_name = player.getName();
    static_cast<void>(getServer().getScheduler().runTaskLater(
        *this,
        [this, player_uuid, player_name] {
            if (!ready_) {
                return;
            }
            auto *current = getServer().getPlayer(player_name);
            if (current == nullptr || current->getUniqueId().str() != player_uuid) {
                return;
            }
            auto &inventory = current->getInventory();
            auto contents = inventory.getContents();
            auto offhand = inventory.getItemInOffHand();
            inventory.setContents(std::move(contents));
            inventory.setItemInOffHand(std::move(offhand));
        },
        1));
}

void ExchangePlugin::giveExchanger(endstone::Player &player) {
    const auto language = languageFor(player);
    endstone::ItemStack stick(endstone::ItemTypeId("minecraft:stick"), 1);
    auto meta = stick.getItemMeta();
    meta->setDisplayName("exchanger");
    meta->setLore(std::vector<std::string>{std::string(messageText(language, Message::ExchangerLore))});
    if (!stick.setItemMeta(meta.get())) {
        throw UserError(tr(language, Message::ExchangerMetaFailed));
    }
    const auto leftovers = player.getInventory().addItem(stick);
    for (const auto &[slot, item] : leftovers) {
        static_cast<void>(slot);
        static_cast<void>(player.getDimension().dropItem(player.getLocation(), item));
    }
    player.sendMessage("{}", messageText(language, Message::ExchangerReceived));
}

void ExchangePlugin::localizeExchangers(endstone::Player &player) {
    auto &inventory = player.getInventory();
    const auto lore = std::vector<std::string>{
        std::string(messageText(languageFor(player), Message::ExchangerLore))};
    const auto update = [&lore](endstone::ItemStack &item) {
        auto meta = item.getItemMeta();
        meta->setLore(lore);
        return item.setItemMeta(meta.get());
    };
    for (int slot = 0; slot < inventory.getSize(); ++slot) {
        auto item = inventory.getItem(slot);
        if (item && isExchanger(item) && update(*item)) {
            inventory.setItem(slot, std::move(item));
        }
    }
    auto offhand = inventory.getItemInOffHand();
    if (offhand && isExchanger(offhand) && update(*offhand)) {
        inventory.setItemInOffHand(std::move(*offhand));
    }
}

void ExchangePlugin::refreshHolograms() {
    if (!ready_ || !hologram_snapshot_state_) {
        return;
    }
    try {
        auto state = hologram_snapshot_state_;
        std::optional<std::unordered_map<Id, OrderBook>> snapshot;
        std::optional<std::string> error;
        {
            std::scoped_lock lock(state->mutex);
            snapshot = std::move(state->ready_snapshot);
            state->ready_snapshot.reset();
            error = std::move(state->error);
            state->error.reset();
        }
        if (error) {
            getLogger().warning("Asynchronous hologram snapshot failed: {}", *error);
        }
        if (snapshot) {
            hologram_books_ = std::move(*snapshot);
            for (const auto &[id, market] : markets_) {
                if (const auto book = hologram_books_.find(id); book != hologram_books_.end()) {
                    refreshHologram(market, book->second);
                }
            }
        }

        std::vector<Id> ids;
        ids.reserve(markets_.size());
        for (const auto &[id, market] : markets_) {
            static_cast<void>(market);
            ids.push_back(id);
        }
        if (ids.empty() || state->query_running.exchange(true)) {
            return;
        }
        const auto database_config = config_.database;
        const auto initial_balance = config_.market.initial_balance_cents;
        const auto max_order_quantity = config_.market.max_order_quantity;
        if (state->query_thread.joinable()) {
            state->query_thread.join();
        }
        auto *state_ptr = state.get();
        try {
            // Endstone 0.11.6's async scheduler captures its queued task by reference before
            // dispatching it to the worker pool, which can dereference a dead queue element.
            // Own one standard C++ worker instead and join it explicitly during shutdown.
            state->query_thread = std::jthread(
                [state_ptr, ids = std::move(ids), database_config, initial_balance, max_order_quantity] {
                    try {
                        Database database(database_config);
                        database.connect();
                        ExchangeService service(database, initial_balance, max_order_quantity);
                        auto books = service.topOfBooks(ids);
                        if (!state_ptr->stopping) {
                            std::scoped_lock lock(state_ptr->mutex);
                            state_ptr->ready_snapshot = std::move(books);
                        }
                    } catch (const std::exception &exception) {
                        if (!state_ptr->stopping) {
                            std::scoped_lock lock(state_ptr->mutex);
                            state_ptr->error = exception.what();
                        }
                    } catch (...) {
                        if (!state_ptr->stopping) {
                            std::scoped_lock lock(state_ptr->mutex);
                            state_ptr->error = "unknown asynchronous snapshot failure";
                        }
                    }
                    state_ptr->query_running = false;
                });
        } catch (...) {
            state->query_running = false;
            throw;
        }
    } catch (const std::exception &error) {
        getLogger().warning("Hologram refresh failed: {}", error.what());
    }
}

void ExchangePlugin::refreshHologram(const Market &market) {
    if (const auto book = hologram_books_.find(market.id); book != hologram_books_.end()) {
        refreshHologram(market, book->second);
    } else {
        refreshHologram(market, OrderBook{});
    }
    refreshHolograms();
}

void ExchangePlugin::refreshHologram(const Market &market, const OrderBook &book) {
    if (!isTargetChunkLoaded(market)) {
        return;
    }
    const auto location = targetLocation(market);
    if (!location) {
        return;
    }
    auto hologram_location = *location;
    if (market.target_kind == TargetKind::Actor) {
        hologram_location.setY(hologram_location.getY() + 2.25F);
    }
    const HologramAnchor current_anchor{hologram_location.getDimension().getName(), hologram_location.getX(),
                                         hologram_location.getY(), hologram_location.getZ()};
    endstone::Actor *hologram = nullptr;
    const auto tag = hologramTag(market.id);
    const auto remembered = hologram_ids_.find(market.id);
    std::vector<endstone::Actor *> matches;
    if (const auto *level = getServer().getLevel(); level != nullptr) {
        for (auto *actor : level->getActors()) {
            if (actor != nullptr && actor->isValid() && hasTag(*actor, tag)) {
                matches.push_back(actor);
                if (remembered != hologram_ids_.end() && actor->getId() == remembered->second) {
                    hologram = actor;
                }
            }
        }
    }
    if (hologram == nullptr && !matches.empty()) {
        hologram = matches.front();
    }
    for (auto *duplicate : matches) {
        if (duplicate != hologram) {
            duplicate->remove();
        }
    }
    if (hologram == nullptr) {
        if (!isTargetChunkSpawnReady(market) || pending_hologram_recreates_.contains(market.id)) {
            return;
        }
        hologram = hologram_location.getDimension().spawnActor(hologram_location, "minecraft:armor_stand");
        if (hologram == nullptr) {
            return;
        }
        static_cast<void>(hologram->addScoreboardTag(tag));
    } else {
        std::optional<HologramAnchor> previous_anchor;
        if (const auto anchor = hologram_anchors_.find(market.id); anchor != hologram_anchors_.end()) {
            previous_anchor = anchor->second;
        } else {
            const auto existing_location = hologram->getLocation();
            previous_anchor = HologramAnchor{existing_location.getDimension().getName(), existing_location.getX(),
                                             existing_location.getY(), existing_location.getZ()};
        }
        if (shouldRepositionHologram(market.target_kind, previous_anchor, current_anchor)) {
            static_cast<void>(hologram->teleport(hologram_location));
        }
    }
    hologram_ids_[market.id] = hologram->getId();
    hologram_anchors_[market.id] = current_anchor;
    const auto fallback_name = marketHologramText(
        market, book, Language::SimplifiedChinese, localizedItemName(market.item, Language::SimplifiedChinese));
    const bool name_changed = hologram->getNameTag() != fallback_name;
    if (name_changed) {
        hologram->setNameTag(fallback_name);
    }
    hologram->setNameTagVisible(true);
    hologram->setNameTagAlwaysVisible(true);
    sendHologramAppearance(*hologram, market, book);
    if (name_changed) {
        static_cast<void>(getServer().getScheduler().runTaskLater(
            *this,
            [this, market_id = market.id] {
                if (!ready_) {
                    return;
                }
                const auto market_it = markets_.find(market_id);
                if (market_it == markets_.end()) {
                    return;
                }
                auto *current = findActorByTag(hologramTag(market_id));
                if (current == nullptr) {
                    return;
                }
                const auto book_it = hologram_books_.find(market_id);
                const auto &current_book = book_it == hologram_books_.end() ? OrderBook{} : book_it->second;
                sendHologramAppearance(*current, market_it->second, current_book);
            },
            1));
    }
}

void ExchangePlugin::sendHologramAppearance(const endstone::Actor &hologram, const Market &market,
                                            const OrderBook &book, endstone::Player *recipient) const {
    if (recipient != nullptr) {
        const auto language = languageFor(*recipient);
        const auto text = marketHologramText(market, book, language, localizedItemName(market.item, language));
        const auto payload = hologramAppearancePacket(hologram.getRuntimeId(), text);
        recipient->sendPacket(SetActorDataPacketId, payload);
        return;
    }
    for (auto *player : getServer().getOnlinePlayers()) {
        if (player != nullptr) {
            const auto language = languageFor(*player);
            const auto text = marketHologramText(market, book, language, localizedItemName(market.item, language));
            const auto payload = hologramAppearancePacket(hologram.getRuntimeId(), text);
            player->sendPacket(SetActorDataPacketId, payload);
        }
    }
}

void ExchangePlugin::queuePlayerHologramSync(endstone::Player &player, const bool recreate_if_pending) {
    const auto player_uuid = player.getUniqueId().str();
    const auto player_name = player.getName();
    for (const auto delay : std::array<std::uint64_t, 4>{1, 20, 60, 120}) {
        static_cast<void>(getServer().getScheduler().runTaskLater(
            *this,
            [this, player_uuid, player_name, recreate_if_pending, delay] {
                if (!ready_) {
                    return;
                }
                auto *current = getServer().getPlayer(player_name);
                if (current != nullptr && current->getUniqueId().str() == player_uuid) {
                    if (recreate_if_pending && delay >= 60 &&
                        pending_join_hologram_recreates_.contains(player_uuid) &&
                        recreateHologramsNearPlayer(*current)) {
                        pending_join_hologram_recreates_.erase(player_uuid);
                        join_loaded_chunks_.erase(player_uuid);
                    }
                    syncHologramsForPlayer(*current);
                }
            },
            delay));
    }
}

bool ExchangePlugin::recreateHologramsNearPlayer(endstone::Player &player) {
    // A server-loaded chunk is not necessarily being tracked by this client yet. Recreating a
    // label before the client has received the old AddActor packet makes the later RemoveActor
    // ineffective for that client, so the stale label can reappear after a teleport. Keep the
    // destructive replacement inside a conservative client-tracking radius; the pending join
    // repair remains armed and PlayerMove queues it again as the player approaches the market.
    constexpr float RecreateRangeSquared = 64.0F * 64.0F;
    const auto player_uuid = player.getUniqueId().str();
    const auto loaded_at_join = join_loaded_chunks_.find(player_uuid);
    if (loaded_at_join == join_loaded_chunks_.end()) {
        return false;
    }
    std::vector<Id> nearby_markets;
    for (const auto &[market_id, market] : markets_) {
        if (!isTargetChunkLoaded(market) || !isTargetChunkSpawnReady(market)) {
            continue;
        }
        const auto location = targetLocation(market);
        if (!location || location->getDimension().getName() != player.getDimension().getName() ||
            location->distanceSquared(player.getLocation()) > RecreateRangeSquared) {
            continue;
        }
        const auto key = chunkKey(location->getDimension().getName(), blockToChunk(location->getBlockX()),
                                  blockToChunk(location->getBlockZ()));
        if (!loaded_at_join->second.contains(key)) {
            continue;
        }
        nearby_markets.push_back(market_id);
    }
    for (const auto market_id : nearby_markets) {
        if (!pending_hologram_recreates_.insert(market_id).second) {
            continue;
        }
        removeHologram(market_id);
        static_cast<void>(getServer().getScheduler().runTaskLater(
            *this,
            [this, market_id] {
                pending_hologram_recreates_.erase(market_id);
                if (!ready_) {
                    return;
                }
                const auto market = markets_.find(market_id);
                if (market == markets_.end() || !isTargetChunkLoaded(market->second) ||
                    !isTargetChunkSpawnReady(market->second)) {
                    return;
                }
                if (const auto book = hologram_books_.find(market_id); book != hologram_books_.end()) {
                    refreshHologram(market->second, book->second);
                } else {
                    refreshHologram(market->second, OrderBook{});
                }
            },
            5));
    }
    return !nearby_markets.empty();
}

void ExchangePlugin::syncHologramsForPlayer(endstone::Player &player) const {
    constexpr float AppearanceRangeSquared = 160.0F * 160.0F;
    for (const auto &[market_id, market] : markets_) {
        endstone::Actor *hologram = nullptr;
        if (const auto remembered = hologram_ids_.find(market_id); remembered != hologram_ids_.end()) {
            hologram = findActorById(remembered->second);
        }
        if (hologram == nullptr) {
            hologram = findActorByTag(hologramTag(market_id));
        }
        if (hologram == nullptr || hologram->getDimension().getName() != player.getDimension().getName() ||
            hologram->getLocation().distanceSquared(player.getLocation()) > AppearanceRangeSquared) {
            continue;
        }
        const auto book = hologram_books_.find(market_id);
        sendHologramAppearance(*hologram, market, book == hologram_books_.end() ? OrderBook{} : book->second,
                               &player);
    }
}

void ExchangePlugin::queueLoadedChunkReconcile(std::string dimension_name, const int chunk_x, const int chunk_z) {
    static_cast<void>(getServer().getScheduler().runTaskLater(
        *this,
        [this, dimension_name, chunk_x, chunk_z] {
            if (ready_ && loaded_chunks_.contains(chunkKey(dimension_name, chunk_x, chunk_z))) {
                reconcileLoadedChunk(dimension_name, chunk_x, chunk_z);
            }
        },
        2));
    static_cast<void>(getServer().getScheduler().runTaskLater(
        *this,
        [this, dimension_name, chunk_x, chunk_z] {
            const auto key = chunkKey(dimension_name, chunk_x, chunk_z);
            if (ready_ && loaded_chunks_.contains(key)) {
                hologram_spawn_ready_chunks_.insert(key);
                reconcileLoadedChunk(dimension_name, chunk_x, chunk_z);
            }
        },
        20));
}

void ExchangePlugin::reconcileLoadedChunk(const std::string_view dimension_name, const int chunk_x,
                                          const int chunk_z) {
    if (const auto *level = getServer().getLevel(); level != nullptr) {
        for (auto *actor : level->getActors()) {
            if (actor == nullptr || !actor->isValid() ||
                lower(actor->getDimension().getName()) != lower(std::string(dimension_name))) {
                continue;
            }
            const auto actor_location = actor->getLocation();
            if (blockToChunk(actor_location.getBlockX()) != chunk_x ||
                blockToChunk(actor_location.getBlockZ()) != chunk_z) {
                continue;
            }
            for (const auto &tag : actor->getScoreboardTags()) {
                if (!tag.starts_with(HologramTagPrefix)) {
                    continue;
                }
                const auto market_id = marketIdFromHologramTag(tag);
                if (!market_id || !markets_.contains(*market_id) ||
                    (markets_.at(*market_id).target_kind == TargetKind::Block &&
                     !marketTargetsChunk(markets_.at(*market_id), dimension_name, chunk_x, chunk_z))) {
                    actor->remove();
                }
                break;
            }
        }
    }

    for (const auto &[market_id, market] : markets_) {
        if (!marketTargetsChunk(market, dimension_name, chunk_x, chunk_z)) {
            continue;
        }
        if (const auto book = hologram_books_.find(market_id); book != hologram_books_.end()) {
            refreshHologram(market, book->second);
        } else {
            refreshHologram(market, OrderBook{});
        }
    }
}

void ExchangePlugin::removeHologram(const Id market_id) {
    const auto tag = hologramTag(market_id);
    std::optional<std::int64_t> remembered;
    if (const auto it = hologram_ids_.find(market_id); it != hologram_ids_.end()) {
        remembered = it->second;
    }
    hologram_ids_.erase(market_id);
    hologram_anchors_.erase(market_id);
    const auto *level = getServer().getLevel();
    if (level == nullptr) {
        return;
    }
    for (auto *actor : level->getActors()) {
        if (actor != nullptr && actor->isValid() &&
            (hasTag(*actor, tag) || (remembered && actor->getId() == *remembered))) {
            actor->remove();
        }
    }
}

void ExchangePlugin::removeAllHolograms() {
    if (const auto *level = getServer().getLevel(); level != nullptr) {
        for (auto *actor : level->getActors()) {
            if (actor == nullptr || !actor->isValid()) {
                continue;
            }
            for (const auto &tag : actor->getScoreboardTags()) {
                if (tag.starts_with(HologramTagPrefix)) {
                    actor->remove();
                    break;
                }
            }
        }
    }
    hologram_ids_.clear();
    hologram_anchors_.clear();
}

endstone::Actor *ExchangePlugin::findActorById(const std::int64_t actor_id) const {
    const auto *level = getServer().getLevel();
    if (level == nullptr) {
        return nullptr;
    }
    for (auto *actor : level->getActors()) {
        if (actor != nullptr && actor->getId() == actor_id && actor->isValid()) {
            return actor;
        }
    }
    return nullptr;
}

endstone::Actor *ExchangePlugin::findActorByTag(const std::string_view tag) const {
    const auto *level = getServer().getLevel();
    if (level == nullptr) {
        return nullptr;
    }
    for (auto *actor : level->getActors()) {
        if (actor != nullptr && actor->isValid() && hasTag(*actor, tag)) {
            return actor;
        }
    }
    return nullptr;
}

std::optional<endstone::Location> ExchangePlugin::targetLocation(const Market &market) const {
    const auto *level = getServer().getLevel();
    if (level == nullptr) {
        return std::nullopt;
    }
    if (market.target_kind == TargetKind::Block) {
        auto *dimension = level->getDimension(market.dimension_name);
        if (dimension == nullptr || !market.block_x || !market.block_y || !market.block_z) {
            return std::nullopt;
        }
        int anchor_x = *market.block_x;
        int anchor_y = *market.block_y;
        int anchor_z = *market.block_z;
        bool unresolved_frame = false;
        const auto block = dimension->getBlockAt(*market.block_x, *market.block_y, *market.block_z);
        if (block && isItemFrameBlock(block->getType())) {
            unresolved_frame = true;
            if (const auto data = block->getData()) {
                const auto states = data->getBlockStates();
                if (const auto facing = states.find("facing_direction"); facing != states.end()) {
                    if (const auto value = std::get_if<int>(&facing->second)) {
                        if (const auto support = itemFrameSupportOffset(*value)) {
                            anchor_x += support->x;
                            anchor_y += support->y;
                            anchor_z += support->z;
                            unresolved_frame = false;
                        }
                    }
                }
            }
        }
        const float label_y = static_cast<float>(anchor_y) + (unresolved_frame ? 2.0F : 1.0F);
        return endstone::Location(*dimension, static_cast<float>(anchor_x) + 0.5F, label_y,
                                  static_cast<float>(anchor_z) + 0.5F);
    }
    if (auto *actor = findActorByTag(targetTag(market.id)); actor != nullptr) {
        return actor->getLocation();
    }
    if (market.actor_id) {
        if (auto *actor = findActorById(*market.actor_id); actor != nullptr) {
            return actor->getLocation();
        }
    }
    return std::nullopt;
}

bool ExchangePlugin::isTargetChunkLoaded(const Market &market) const {
    if (market.target_kind == TargetKind::Block) {
        return market.block_x && market.block_z &&
               loaded_chunks_.contains(chunkKey(market.dimension_name, blockToChunk(*market.block_x),
                                                blockToChunk(*market.block_z)));
    }
    const auto location = targetLocation(market);
    return location && loaded_chunks_.contains(
                           chunkKey(location->getDimension().getName(), blockToChunk(location->getBlockX()),
                                    blockToChunk(location->getBlockZ())));
}

bool ExchangePlugin::isTargetChunkSpawnReady(const Market &market) const {
    if (market.target_kind == TargetKind::Block) {
        return market.block_x && market.block_z &&
               hologram_spawn_ready_chunks_.contains(chunkKey(
                   market.dimension_name, blockToChunk(*market.block_x), blockToChunk(*market.block_z)));
    }
    const auto location = targetLocation(market);
    return location && hologram_spawn_ready_chunks_.contains(
                           chunkKey(location->getDimension().getName(), blockToChunk(location->getBlockX()),
                                    blockToChunk(location->getBlockZ())));
}

bool ExchangePlugin::marketTargetsChunk(const Market &market, const std::string_view dimension_name,
                                        const int chunk_x, const int chunk_z) const {
    if (market.target_kind == TargetKind::Block) {
        return market.block_x && market.block_z && lower(market.dimension_name) == lower(std::string(dimension_name)) &&
               blockToChunk(*market.block_x) == chunk_x && blockToChunk(*market.block_z) == chunk_z;
    }
    const auto location = targetLocation(market);
    return location && lower(location->getDimension().getName()) == lower(std::string(dimension_name)) &&
           blockToChunk(location->getBlockX()) == chunk_x && blockToChunk(location->getBlockZ()) == chunk_z;
}

Language ExchangePlugin::languageFor(const endstone::CommandSender &sender) const noexcept {
    const auto *player = sender.asPlayer();
    return player == nullptr ? Language::SimplifiedChinese : languageFromLocale(player->getLocale());
}

std::string ExchangePlugin::localizedItemName(const ItemPrototype &item, const Language language) const {
    try {
        endstone::ItemStack sample(endstone::ItemTypeId(item.type), 1, item.data);
        if (!item.nbt.empty()) {
            sample.setNbt(NbtCodec::decode(item.nbt));
        }
        if (const auto meta = sample.getItemMeta(); meta && meta->hasDisplayName()) {
            return item.name;
        }
        const auto key = sample.getTranslationKey();
        auto translated = getServer().getLanguage().translate(key, std::string(localeCode(language)));
        if (!translated.empty() && translated != key) {
            return translated;
        }
    } catch (const std::exception &) {
        // The persisted fallback remains usable if a newer item type is unknown to this server.
    }
    return item.name;
}

std::string ExchangePlugin::formatMoney(const Cents cents) {
    return std::format("{:.2f}", static_cast<double>(cents) / 100.0);
}

std::string ExchangePlugin::targetTag(const Id market_id) {
    return std::string(TargetTagPrefix) + std::to_string(market_id);
}

std::string ExchangePlugin::hologramTag(const Id market_id) {
    return std::string(HologramTagPrefix) + std::to_string(market_id);
}

std::string ExchangePlugin::chunkKey(const std::string_view dimension_name, const int chunk_x, const int chunk_z) {
    return std::format("{}|{}|{}", lower(std::string(dimension_name)), chunk_x, chunk_z);
}

std::optional<Id> ExchangePlugin::marketIdFromHologramTag(const std::string_view tag) {
    if (!tag.starts_with(HologramTagPrefix)) {
        return std::nullopt;
    }
    const auto value = tag.substr(HologramTagPrefix.size());
    Id market_id{};
    const auto *begin = value.data();
    const auto *end = value.data() + value.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, market_id);
    if (error != std::errc{} || parsed_end != end || market_id <= 0) {
        return std::nullopt;
    }
    return market_id;
}

bool ExchangePlugin::hasTag(const endstone::Actor &actor, const std::string_view tag) {
    const auto tags = actor.getScoreboardTags();
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

} // namespace exchange

ENDSTONE_PLUGIN("exchange", ENDSTONE_EXCHANGE_VERSION, exchange::ExchangePlugin) {
    prefix = "Exchange";
    description = "Trade matching items through world objects with MySQL persistence";
    website = "https://github.com/wingxia/endstone-exchange";
    authors = {"Wing Xia"};

    command("exchange")
        .description("Open item trading and manage trade points")
        .usages("/exchange", "/exchange give [player: str]", "/exchange balance", "/exchange orders", "/exchange claim",
                "/exchange addbalance <player: str> <amount: float>", "/exchange status")
        .permissions("exchange.use");

    permission("exchange.use")
        .description("Trade items and manage personal orders")
        .default_(endstone::PermissionDefault::True);
    permission("exchange.admin")
        .description("Create trade points and administer balances")
        .default_(endstone::PermissionDefault::Operator);
}
