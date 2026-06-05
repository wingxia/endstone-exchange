from __future__ import annotations

import json
import threading
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler
from typing import Any
from urllib.parse import parse_qs, urlparse


@dataclass
class BridgeState:
    token: str
    umoney: Any
    lock: threading.Lock = field(default_factory=threading.Lock)
    idempotency: dict[str, dict[str, Any]] = field(default_factory=dict)

    def raw_balance(self, player: str) -> int | None:
        value = self.umoney.api_get_player_money(player)
        if value is None:
            return None
        return int(value)

    def balance(self, player: str) -> int:
        value = self.raw_balance(player)
        if value is None:
            return 0
        return value

    def debit(self, player: str, amount: int, key: str) -> dict[str, Any]:
        with self.lock:
            if key in self.idempotency:
                return self.idempotency[key]
            current = self.raw_balance(player)
            if current is None:
                raise ValueError("player data not found")
            if current < amount:
                raise ValueError("insufficient funds")
            self.umoney.api_change_player_money(player, -amount)
            response = {"ok": True, "balance": self.balance(player)}
            self.idempotency[key] = response
            return response

    def credit(self, player: str, amount: int, key: str) -> dict[str, Any]:
        with self.lock:
            if key in self.idempotency:
                return self.idempotency[key]
            if self.raw_balance(player) is None:
                raise ValueError("player data not found")
            self.umoney.api_change_player_money(player, amount)
            response = {"ok": True, "balance": self.balance(player)}
            self.idempotency[key] = response
            return response


def make_handler(state: BridgeState):
    class Handler(BaseHTTPRequestHandler):
        server_version = "ExchangeUMoneyBridge/0.1"

        def do_GET(self) -> None:
            if not self._authorized():
                return self._json(401, {"ok": False, "error": "unauthorized"})
            parsed = urlparse(self.path)
            if parsed.path != "/balance":
                return self._json(404, {"ok": False, "error": "not found"})
            player = parse_qs(parsed.query).get("player", [""])[0]
            if not player:
                return self._json(400, {"ok": False, "error": "player is required"})
            return self._json(200, {"ok": True, "balance": state.balance(player)})

        def do_POST(self) -> None:
            if not self._authorized():
                return self._json(401, {"ok": False, "error": "unauthorized"})
            parsed = urlparse(self.path)
            if parsed.path not in {"/debit", "/credit"}:
                return self._json(404, {"ok": False, "error": "not found"})
            try:
                payload = self._read_json()
                player = str(payload["player"])
                amount = int(payload["amount"])
                key = str(payload["idempotency_key"])
                if amount <= 0:
                    raise ValueError("amount must be positive")
                response = state.debit(player, amount, key) if parsed.path == "/debit" else state.credit(player, amount, key)
            except KeyError as exc:
                return self._json(400, {"ok": False, "error": f"missing {exc.args[0]}"})
            except ValueError as exc:
                return self._json(400, {"ok": False, "error": str(exc)})
            return self._json(200, response)

        def log_message(self, _format: str, *_args: Any) -> None:
            return None

        def _authorized(self) -> bool:
            expected = f"Bearer {state.token}"
            return self.headers.get("Authorization") == expected

        def _read_json(self) -> dict[str, Any]:
            length = int(self.headers.get("Content-Length") or "0")
            data = self.rfile.read(length)
            return json.loads(data.decode("utf-8") or "{}")

        def _json(self, status: int, payload: dict[str, Any]) -> None:
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    return Handler
