from endstone.command import Command, CommandSender
from endstone.plugin import Plugin


class ExchangeE2EDriver(Plugin):
    """Dispatch a small command allowlist as an online player on an isolated server."""

    api_version = "0.11"

    commands = {
        "exchangee2e": {
            "description": "Dispatch an Exchange E2E command as an online player.",
            "usages": [
                "/exchangee2e <player: str> balance",
                "/exchangee2e <player: str> status",
                "/exchangee2e <player: str> orders",
                "/exchangee2e <player: str> claim",
                "/exchangee2e <player: str> give",
                "/exchangee2e <player: str> deposit <amount: int>",
                "/exchangee2e <player: str> withdraw <amount: int>",
                "/exchangee2e <player: str> addbalance <target: str> <amount: float>",
            ],
            "permissions": ["exchange.e2e.dispatch"],
        }
    }

    permissions = {
        "exchange.e2e.dispatch": {
            "description": "Dispatch isolated Exchange E2E commands.",
            "default": True,
        }
    }

    _allowed = {
        "balance",
        "status",
        "orders",
        "claim",
        "give",
        "deposit",
        "withdraw",
        "addbalance",
    }

    def on_enable(self) -> None:
        self.logger.warning(
            "Exchange E2E driver enabled; install this plugin only on an isolated test server."
        )

    def on_command(
        self, sender: CommandSender, command: Command, args: list[str]
    ) -> bool:
        if command.name != "exchangee2e":
            return False
        if len(args) < 2 or args[1].lower() not in self._allowed:
            sender.send_error_message("Invalid Exchange E2E command.")
            return True

        player = self.server.get_player(args[0])
        if player is None:
            sender.send_error_message(f"E2E player is not online: {args[0]}")
            return True

        command_line = f"exchange {' '.join(args[1:])}"
        dispatched = self.server.dispatch_command(player, command_line)
        sender.send_message(
            f"E2E_DISPATCH player={player.name} command={command_line} result={dispatched}"
        )
        return True
