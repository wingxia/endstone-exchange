from __future__ import annotations

import os
import secrets
import threading
from http.server import ThreadingHTTPServer
from pathlib import Path

import yaml

from .server import BridgeState, make_handler

try:
    from endstone.plugin import Plugin
except Exception:  # pragma: no cover - only used outside Endstone in import tests
    class Plugin:  # type: ignore[no-redef]
        def __init__(self) -> None:
            self.server = None
            self.logger = type("Logger", (), {"info": print, "warning": print, "error": print})()


DEFAULT_CONFIG = {
    "host": "127.0.0.1",
    "port": 8765,
    "token": "",
    "umoney_plugin": "umoney",
}


class UMoneyBridgePlugin(Plugin):
    api_version = "0.11"

    def __init__(self) -> None:
        super().__init__()
        self._httpd: ThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None

    def on_enable(self) -> None:
        config = self._load_config()
        token = str(config["token"])
        umoney = self.server.plugin_manager.get_plugin(str(config["umoney_plugin"]))
        if umoney is None:
            raise RuntimeError("UMoney plugin is not loaded")
        state = BridgeState(token=token, umoney=umoney)
        self._httpd = ThreadingHTTPServer((str(config["host"]), int(config["port"])), make_handler(state))
        self._thread = threading.Thread(target=self._httpd.serve_forever, name="exchange-umoney-bridge", daemon=True)
        self._thread.start()
        self.logger.info(f"Exchange UMoney bridge listening on {config['host']}:{config['port']}")

    def on_disable(self) -> None:
        if self._httpd is not None:
            self._httpd.shutdown()
            self._httpd.server_close()
        if self._thread is not None:
            self._thread.join(timeout=3)

    def _load_config(self) -> dict:
        data_dir = Path(os.getcwd()) / "plugins" / "exchange_umoney_bridge"
        data_dir.mkdir(parents=True, exist_ok=True)
        path = data_dir / "config.yml"
        if path.exists():
            config = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        else:
            config = dict(DEFAULT_CONFIG)
            config["token"] = secrets.token_urlsafe(32)
            path.write_text(yaml.safe_dump(config, allow_unicode=True, sort_keys=False), encoding="utf-8")
            self.logger.warning(f"Generated UMoney bridge token in {path}")
        merged = dict(DEFAULT_CONFIG)
        merged.update(config)
        if not merged["token"]:
            merged["token"] = secrets.token_urlsafe(32)
            path.write_text(yaml.safe_dump(merged, allow_unicode=True, sort_keys=False), encoding="utf-8")
        return merged

