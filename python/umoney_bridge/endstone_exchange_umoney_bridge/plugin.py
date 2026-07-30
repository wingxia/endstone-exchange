from __future__ import annotations

import json
import os
import secrets
import threading
from http.server import ThreadingHTTPServer
from pathlib import Path
from typing import Any

from endstone.plugin import Plugin

from .server import BridgeJournal, BridgeState, MainThreadDispatcher, make_handler


DEFAULT_CONFIG = {
    "host": "127.0.0.1",
    "port": 8765,
    "token": "",
    "umoney_plugin": "umoney",
    "umoney_money_file": "plugins/umoney/money.json",
    "main_thread_timeout_seconds": 3.0,
    "test_after_apply_delay_milliseconds": 0,
    "test_response_delay_milliseconds": 0,
}


class UMoneyBridgePlugin(Plugin):
    api_version = "0.11"

    def __init__(self) -> None:
        super().__init__()
        self._httpd: ThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None
        self._task: Any = None
        self._dispatcher: MainThreadDispatcher | None = None
        self._journal: BridgeJournal | None = None
        self._umoney_plugin_name = "umoney"
        self._umoney_money_file = Path("plugins/umoney/money.json")

    def on_enable(self) -> None:
        config, data_dir = self._load_config()
        self._umoney_plugin_name = str(config["umoney_plugin"])
        configured_money_file = Path(str(config["umoney_money_file"]))
        self._umoney_money_file = (
            configured_money_file
            if configured_money_file.is_absolute()
            else Path.cwd() / configured_money_file
        )
        self._journal = BridgeJournal(data_dir / "idempotency.sqlite3")
        self._dispatcher = MainThreadDispatcher(float(config["main_thread_timeout_seconds"]))
        self._task = self.server.scheduler.run_task(self, self._drain, delay=1, period=1)
        state = BridgeState(
            str(config["token"]),
            self._get_umoney,
            self._journal,
            self._dispatcher,
            durability_barrier=self._sync_umoney_money_file,
            after_apply_delay_seconds=float(config["test_after_apply_delay_milliseconds"]) / 1000.0,
            response_delay_seconds=float(config["test_response_delay_milliseconds"]) / 1000.0,
        )
        self._httpd = ThreadingHTTPServer((str(config["host"]), int(config["port"])), make_handler(state))
        self._thread = threading.Thread(
            target=self._httpd.serve_forever,
            name="exchange-umoney-bridge",
            daemon=True,
        )
        self._thread.start()
        self.logger.info(
            f"Exchange UMoney bridge listening on {config['host']}:{config['port']}"
        )

    def on_disable(self) -> None:
        if self._httpd is not None:
            self._httpd.shutdown()
            self._httpd.server_close()
            self._httpd = None
        if self._thread is not None:
            self._thread.join(timeout=4)
            self._thread = None
        if self._task is not None:
            self._task.cancel()
            self._task = None
        if self._dispatcher is not None:
            self._dispatcher.close()
            self._dispatcher = None
        if self._journal is not None:
            self._journal.close()
            self._journal = None

    def _drain(self) -> None:
        if self._dispatcher is not None:
            self._dispatcher.drain()

    def _get_umoney(self) -> Any:
        return self.server.plugin_manager.get_plugin(self._umoney_plugin_name)

    def _sync_umoney_money_file(self) -> None:
        descriptor = os.open(self._umoney_money_file, os.O_RDONLY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)

    def _load_config(self) -> tuple[dict[str, Any], Path]:
        data_dir = Path(self.data_folder)
        data_dir.mkdir(parents=True, exist_ok=True)
        path = data_dir / "config.json"
        if path.exists():
            config = json.loads(path.read_text(encoding="utf-8"))
        else:
            config = dict(DEFAULT_CONFIG)
            config["token"] = secrets.token_urlsafe(32)
            self._write_config(path, config)
            self.logger.warning(f"Generated UMoney bridge token in {path}")
        merged = dict(DEFAULT_CONFIG)
        merged.update(config)
        if not merged["token"]:
            merged["token"] = secrets.token_urlsafe(32)
            self._write_config(path, merged)
        if str(merged["host"]) not in {"127.0.0.1", "::1", "localhost"}:
            raise RuntimeError("the UMoney bridge must bind to a loopback host")
        token = str(merged["token"])
        if len(token) < 32 or "\r" in token or "\n" in token:
            raise RuntimeError(
                "the UMoney bridge token must contain at least 32 characters and no newlines"
            )
        try:
            port = int(merged["port"])
            timeout = float(merged["main_thread_timeout_seconds"])
            after_apply_delay = int(merged["test_after_apply_delay_milliseconds"])
            response_delay = int(merged["test_response_delay_milliseconds"])
        except (TypeError, ValueError) as error:
            raise RuntimeError("the UMoney bridge numeric settings are invalid") from error
        if not 1 <= port <= 65_535 or not 0.1 <= timeout <= 30:
            raise RuntimeError("the UMoney bridge port or main-thread timeout is invalid")
        if (
            after_apply_delay < 0
            or response_delay < 0
            or after_apply_delay > 60_000
            or response_delay > 60_000
        ):
            raise RuntimeError("the UMoney bridge test delays are invalid")
        if not str(merged["umoney_plugin"]):
            raise RuntimeError("the UMoney plugin name must not be empty")
        try:
            os.chmod(path, 0o600)
        except OSError:
            pass
        return merged, data_dir

    @staticmethod
    def _write_config(path: Path, config: dict[str, Any]) -> None:
        path.write_text(
            json.dumps(config, indent=4, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        try:
            os.chmod(path, 0o600)
        except OSError:
            pass
