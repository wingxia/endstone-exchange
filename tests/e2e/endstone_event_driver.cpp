#include <charconv>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "endstone/command/command.h"
#include "endstone/command/command_sender.h"
#include "endstone/event/block/block_break_event.h"
#include "endstone/event/block/block_place_event.h"
#include "endstone/event/player/player_interact_event.h"
#include "endstone/inventory/meta/item_meta.h"
#include "endstone/level/dimension.h"
#include "endstone/plugin/plugin.h"
#include "endstone/plugin/plugin_manager.h"

namespace exchange::e2e {
namespace {

std::optional<int> parseInt(const std::string_view value) {
    int result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

} // namespace

class ExchangeEventE2EPlugin : public endstone::Plugin {
  public:
    void onEnable() override {
        getLogger().warning(
            "Exchange event E2E driver enabled; install this plugin only on an isolated test server.");
    }

    bool onCommand(endstone::CommandSender &sender, const endstone::Command &command,
                   const std::vector<std::string> &args) override {
        if (command.getName() != "exchangeevente2e") {
            return false;
        }
        if (args.size() != 5) {
            sender.sendErrorMessage(
                "{}", "Usage: /exchangeevente2e <player> <right|left|exchanger|price|break|place> <x> <y> <z>");
            return true;
        }
        auto *player = getServer().getPlayer(args[0]);
        const auto action_name = args[1];
        const auto x = parseInt(args[2]);
        const auto y = parseInt(args[3]);
        const auto z = parseInt(args[4]);
        if (player == nullptr || !x || !y || !z ||
            (action_name != "right" && action_name != "left" && action_name != "exchanger" &&
             action_name != "price" && action_name != "break" && action_name != "place")) {
            sender.sendErrorMessage("{}", "E2E player, action, or coordinates are invalid.");
            return true;
        }

        auto block = player->getDimension().getBlockAt(*x, *y, *z);
        if (!block) {
            sender.sendErrorMessage("{}", "The E2E block is not in a loaded chunk.");
            return true;
        }
        if (action_name == "break") {
            endstone::BlockBreakEvent event(std::move(block), *player);
            getServer().getPluginManager().callEvent(event);
            if (!event.isCancelled()) {
                event.getBlock().setType("minecraft:air", false);
            }
            sender.sendMessage(
                "{}", std::format("E2E_EVENT player={} action={} block={},{},{} cancelled={} resulting={}",
                                  player->getName(), action_name, *x, *y, *z, event.isCancelled(),
                                  event.getBlock().getType()));
            return true;
        }
        if (action_name == "place") {
            auto placed_state = block->captureState();
            auto replaced_block = block->clone();
            auto placed_against = block->getRelative(0, -1, 0);
            if (!placed_state || !replaced_block || !placed_against) {
                sender.sendErrorMessage("{}", "Could not capture the E2E placement blocks.");
                return true;
            }
            placed_state->setType("minecraft:stone");
            endstone::BlockPlaceEvent event(std::move(placed_state), std::move(replaced_block),
                                            std::move(placed_against), *player);
            getServer().getPluginManager().callEvent(event);
            if (!event.isCancelled()) {
                static_cast<void>(event.getBlockPlacedState().update(true, false));
            }
            const auto resulting_block = player->getDimension().getBlockAt(*x, *y, *z);
            sender.sendMessage(
                "{}", std::format("E2E_EVENT player={} action={} block={},{},{} cancelled={} resulting={}",
                                  player->getName(), action_name, *x, *y, *z, event.isCancelled(),
                                  resulting_block ? resulting_block->getType() : "unloaded"));
            return true;
        }
        std::optional<endstone::ItemStack> item;
        if (action_name == "exchanger" || action_name == "price") {
            endstone::ItemStack stick(endstone::ItemTypeId("minecraft:stick"), 1);
            auto meta = stick.getItemMeta();
            meta->setDisplayName(action_name);
            if (!stick.setItemMeta(meta.get())) {
                sender.sendErrorMessage("{}", "Could not create the named E2E stick.");
                return true;
            }
            item = std::move(stick);
        }
        const auto action = action_name == "left"
                                ? endstone::PlayerInteractEvent::Action::LeftClickBlock
                                : endstone::PlayerInteractEvent::Action::RightClickBlock;
        endstone::PlayerInteractEvent event(
            *player, action, std::move(item),
            block.get(), endstone::BlockFace::North, endstone::Vector{0.5F, 0.5F, 0.5F});
        getServer().getPluginManager().callEvent(event);
        sender.sendMessage(
            "{}", std::format("E2E_EVENT player={} action={} block={},{},{} cancelled={}",
                              player->getName(), action_name, *x, *y, *z, event.isCancelled()));
        return true;
    }
};

} // namespace exchange::e2e

ENDSTONE_PLUGIN("exchange_event_e2e", "0.3.0", exchange::e2e::ExchangeEventE2EPlugin) {
    prefix = "ExchangeEventE2E";
    description = "Isolated Endstone event driver for Exchange E2E tests";
    authors = {"Wing Xia"};

    command("exchangeevente2e")
        .description("Dispatch a block interaction through Endstone's event pipeline")
        .usages("/exchangeevente2e <player: str> <action: str> <x: int> <y: int> <z: int>")
        .permissions("exchange.e2e.event");

    permission("exchange.e2e.event")
        .description("Dispatch isolated Exchange E2E events")
        .default_(endstone::PermissionDefault::Operator);
}
