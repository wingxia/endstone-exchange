#include <charconv>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "endstone/command/command.h"
#include "endstone/command/command_sender.h"
#include "endstone/event/player/player_interact_event.h"
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
        if (args.size() != 4) {
            sender.sendErrorMessage("{}", "Usage: /exchangeevente2e <player> <x> <y> <z>");
            return true;
        }
        auto *player = getServer().getPlayer(args[0]);
        const auto x = parseInt(args[1]);
        const auto y = parseInt(args[2]);
        const auto z = parseInt(args[3]);
        if (player == nullptr || !x || !y || !z) {
            sender.sendErrorMessage("{}", "E2E player must be online and coordinates must be integers.");
            return true;
        }

        auto block = player->getDimension().getBlockAt(*x, *y, *z);
        endstone::PlayerInteractEvent event(
            *player, endstone::PlayerInteractEvent::Action::RightClickBlock, std::nullopt,
            block.get(), endstone::BlockFace::North, endstone::Vector{0.5F, 0.5F, 0.5F});
        getServer().getPluginManager().callEvent(event);
        sender.sendMessage(
            "{}", std::format("E2E_EVENT player={} block={},{},{} cancelled={}",
                              player->getName(), *x, *y, *z, event.isCancelled()));
        return true;
    }
};

} // namespace exchange::e2e

ENDSTONE_PLUGIN("exchange_event_e2e", "0.1.0", exchange::e2e::ExchangeEventE2EPlugin) {
    prefix = "ExchangeEventE2E";
    description = "Isolated Endstone event driver for Exchange E2E tests";
    authors = {"Wing Xia"};

    command("exchangeevente2e")
        .description("Dispatch a block interaction through Endstone's event pipeline")
        .usages("/exchangeevente2e <player: str> <x: int> <y: int> <z: int>")
        .permissions("exchange.e2e.event");

    permission("exchange.e2e.event")
        .description("Dispatch isolated Exchange E2E events")
        .default_(endstone::PermissionDefault::Operator);
}
