import os
import signal

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
                "/exchangee2e <player: str> addbalance <target: str> <amount: float>",
            ],
            "permissions": ["exchange.e2e.dispatch"],
        },
        "umoneye2e": {
            "description": "Inspect or set UMoney through its public API on an isolated server.",
            "usages": [
                "/umoneye2e get <player: str>",
                "/umoneye2e set <player: str> <amount: int>",
            ],
            "permissions": ["exchange.e2e.dispatch"],
        },
        "umoneyfaulte2e": {
            "description": "Kill the isolated server after the next durable UMoney mutation.",
            "usages": ["/umoneyfaulte2e arm"],
            "permissions": ["exchange.e2e.dispatch"],
        },
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
        "addbalance",
    }

    def on_enable(self) -> None:
        self.logger.warning(
            "Exchange E2E driver enabled; install this plugin only on an isolated test server."
        )

    def on_command(
        self, sender: CommandSender, command: Command, args: list[str]
    ) -> bool:
        if command.name == "umoneye2e":
            return self._on_umoney_command(sender, args)
        if command.name == "umoneyfaulte2e":
            return self._on_umoney_fault_command(sender, args)
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

    def _umoney(self):
        plugin = self.server.plugin_manager.get_plugin("umoney")
        if plugin is None or not plugin.is_enabled:
            raise RuntimeError("UMoney is not enabled")
        return plugin

    def _on_umoney_command(self, sender: CommandSender, args: list[str]) -> bool:
        try:
            if len(args) not in (2, 3) or args[0].lower() not in {"get", "set"}:
                sender.send_error_message("Usage: umoneye2e <get|set> <player> [amount]")
                return True
            plugin = self._umoney()
            player_name = args[1]
            current = plugin.api_get_player_money(player_name)
            if current is None:
                sender.send_error_message(f"UMoney account does not exist: {player_name}")
                return True
            if args[0].lower() == "set":
                if len(args) != 3:
                    sender.send_error_message("Usage: umoneye2e set <player> <amount>")
                    return True
                target = int(args[2])
                if target < 0:
                    raise ValueError("target balance cannot be negative")
                if target != current:
                    plugin.api_change_player_money(player_name, target - current)
                current = plugin.api_get_player_money(player_name)
            sender.send_message(f"UMONEY_E2E player={player_name} balance={current}")
        except Exception as error:
            sender.send_error_message(f"UMoney E2E failed: {error}")
        return True

    def _on_umoney_fault_command(self, sender: CommandSender, args: list[str]) -> bool:
        try:
            if args != ["arm"]:
                sender.send_error_message("Usage: umoneyfaulte2e arm")
                return True
            plugin = self._umoney()
            if getattr(plugin, "_exchange_e2e_fault_armed", False):
                sender.send_error_message("UMoney fault injection is already armed")
                return True
            original = plugin.api_change_player_money

            def crash_after_durable_change(player_name: str, delta: int) -> None:
                original(player_name, delta)
                money_path = os.path.join(str(plugin.data_folder), "money.json")
                with open(money_path, "rb") as money_file:
                    os.fsync(money_file.fileno())
                directory_fd = os.open(str(plugin.data_folder), os.O_RDONLY)
                try:
                    os.fsync(directory_fd)
                finally:
                    os.close(directory_fd)
                os.kill(os.getpid(), signal.SIGKILL)

            plugin.api_change_player_money = crash_after_durable_change
            plugin._exchange_e2e_fault_armed = True
            sender.send_message("UMONEY_FAULT_E2E armed=1")
        except Exception as error:
            sender.send_error_message(f"UMoney fault E2E failed: {error}")
        return True
