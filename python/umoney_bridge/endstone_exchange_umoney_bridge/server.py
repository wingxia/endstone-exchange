from __future__ import annotations

import hmac
import json
import queue
import sqlite3
import threading
import time
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler
from pathlib import Path
from typing import Any, Callable, TypeVar
from urllib.parse import parse_qs, urlparse


T = TypeVar("T")


class BridgeOperationError(RuntimeError):
    def __init__(
        self,
        message: str,
        *,
        code: str,
        status: int,
        retryable: bool,
        applied: bool = False,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.status = status
        self.retryable = retryable
        self.applied = applied


class DirectDispatcher:
    def call(self, callback: Callable[[], T]) -> T:
        return callback()

    def drain(self) -> None:
        return None

    def close(self) -> None:
        return None


@dataclass
class _DispatchJob:
    callback: Callable[[], Any]
    event: threading.Event = field(default_factory=threading.Event)
    result: Any = None
    error: BaseException | None = None


class MainThreadDispatcher:
    def __init__(self, timeout_seconds: float = 3.0) -> None:
        self._timeout_seconds = timeout_seconds
        self._jobs: queue.Queue[_DispatchJob] = queue.Queue()
        self._accepting = True
        self._lock = threading.Lock()

    def call(self, callback: Callable[[], T]) -> T:
        with self._lock:
            if not self._accepting:
                raise BridgeOperationError(
                    "the UMoney bridge is stopping",
                    code="bridge_stopping",
                    status=503,
                    retryable=True,
                )
            job = _DispatchJob(callback)
            self._jobs.put(job)
        if not job.event.wait(self._timeout_seconds):
            raise BridgeOperationError(
                "the UMoney main-thread operation timed out",
                code="main_thread_timeout",
                status=503,
                retryable=True,
            )
        if job.error is not None:
            raise job.error
        return job.result

    def drain(self, maximum_jobs: int = 64) -> None:
        for _ in range(maximum_jobs):
            try:
                job = self._jobs.get_nowait()
            except queue.Empty:
                return
            try:
                job.result = job.callback()
            except BaseException as error:  # propagate the original API failure to the HTTP request
                job.error = error
            finally:
                job.event.set()

    def close(self) -> None:
        with self._lock:
            self._accepting = False
        while True:
            try:
                job = self._jobs.get_nowait()
            except queue.Empty:
                break
            job.error = BridgeOperationError(
                "the UMoney bridge stopped before the operation ran",
                code="bridge_stopping",
                status=503,
                retryable=True,
            )
            job.event.set()


class BridgeJournal:
    def __init__(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        # Production calls are serialized onto the Endstone main thread. Allowing the
        # test dispatcher to use that same connection from its HTTP worker also keeps
        # the bridge core independently testable without weakening runtime ordering.
        self._connection = sqlite3.connect(path, check_same_thread=False)
        self._connection.row_factory = sqlite3.Row
        self._connection.execute("PRAGMA journal_mode=WAL")
        self._connection.execute("PRAGMA synchronous=FULL")
        self._connection.execute(
            """
            CREATE TABLE IF NOT EXISTS operations (
                idempotency_key TEXT NOT NULL PRIMARY KEY,
                operation TEXT NOT NULL,
                player TEXT NOT NULL,
                amount INTEGER NOT NULL,
                state TEXT NOT NULL,
                balance_before INTEGER NOT NULL,
                balance_after INTEGER NOT NULL,
                created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
                completed_at TEXT
            )
            """
        )
        self._connection.commit()

    def close(self) -> None:
        self._connection.close()

    @staticmethod
    def _raw_balance(umoney: Any, player: str) -> int:
        value = umoney.api_get_player_money(player)
        if value is None:
            raise BridgeOperationError(
                "UMoney player data was not found",
                code="player_not_found",
                status=404,
                retryable=False,
            )
        if type(value) is not int:
            raise BridgeOperationError(
                "UMoney returned a non-integer balance",
                code="invalid_balance",
                status=502,
                retryable=True,
            )
        return value

    def balance(self, umoney: Any, player: str) -> int:
        return self._raw_balance(umoney, player)

    def apply(
        self,
        umoney: Any,
        operation: str,
        player: str,
        amount: int,
        idempotency_key: str,
        *,
        durability_barrier: Callable[[], None] | None = None,
        after_apply_delay_seconds: float = 0,
    ) -> int:
        if operation not in {"debit", "credit"}:
            raise ValueError("unsupported bridge operation")
        if not player or not idempotency_key:
            raise BridgeOperationError(
                "player and idempotency_key are required",
                code="invalid_request",
                status=400,
                retryable=False,
            )
        if amount <= 0:
            raise BridgeOperationError(
                "amount must be a positive integer",
                code="invalid_amount",
                status=400,
                retryable=False,
            )

        row = self._connection.execute(
            "SELECT * FROM operations WHERE idempotency_key=?",
            (idempotency_key,),
        ).fetchone()
        if row is not None:
            if row["operation"] != operation or row["player"] != player or row["amount"] != amount:
                raise BridgeOperationError(
                    "idempotency key was already used for a different operation",
                    code="idempotency_conflict",
                    status=409,
                    retryable=False,
                )
            if row["state"] == "COMPLETED":
                return int(row["balance_after"])
            balance_before = int(row["balance_before"])
            balance_after = int(row["balance_after"])
            current = self._raw_balance(umoney, player)
            if current == balance_after:
                self._run_durability_barrier(durability_barrier)
                self._complete(idempotency_key)
                return balance_after
            if current != balance_before:
                raise BridgeOperationError(
                    "UMoney balance changed while recovering a prepared operation",
                    code="manual_reconciliation_required",
                    status=409,
                    retryable=False,
                )
        else:
            balance_before = self._raw_balance(umoney, player)
            if operation == "debit" and balance_before < amount:
                raise BridgeOperationError(
                    "insufficient UMoney balance",
                    code="insufficient_funds",
                    status=409,
                    retryable=False,
                )
            balance_after = balance_before - amount if operation == "debit" else balance_before + amount
            self._connection.execute(
                """
                INSERT INTO operations(
                    idempotency_key,operation,player,amount,state,balance_before,balance_after
                ) VALUES (?,?,?,?,?,?,?)
                """,
                (idempotency_key, operation, player, amount, "PREPARED", balance_before, balance_after),
            )
            self._connection.commit()

        change = -amount if operation == "debit" else amount
        umoney.api_change_player_money(player, change)
        current = self._raw_balance(umoney, player)
        if current != balance_after:
            raise BridgeOperationError(
                "UMoney did not reach the expected balance",
                code="external_apply_mismatch",
                status=502,
                retryable=True,
                applied=current != balance_before,
            )
        self._run_durability_barrier(durability_barrier)
        if after_apply_delay_seconds > 0:
            time.sleep(after_apply_delay_seconds)
        self._complete(idempotency_key)
        return balance_after

    @staticmethod
    def _run_durability_barrier(callback: Callable[[], None] | None) -> None:
        if callback is None:
            return
        try:
            callback()
        except Exception as error:
            raise BridgeOperationError(
                "UMoney balance could not be flushed to durable storage",
                code="durability_barrier_failed",
                status=503,
                retryable=True,
            ) from error

    def _complete(self, idempotency_key: str) -> None:
        self._connection.execute(
            """
            UPDATE operations
            SET state='COMPLETED',completed_at=CURRENT_TIMESTAMP
            WHERE idempotency_key=? AND state='PREPARED'
            """,
            (idempotency_key,),
        )
        self._connection.commit()


class BridgeState:
    def __init__(
        self,
        token: str,
        umoney_provider: Callable[[], Any],
        journal: BridgeJournal,
        dispatcher: DirectDispatcher | MainThreadDispatcher,
        *,
        durability_barrier: Callable[[], None] | None = None,
        after_apply_delay_seconds: float = 0,
        response_delay_seconds: float = 0,
    ) -> None:
        self.token = token
        self._umoney_provider = umoney_provider
        self._journal = journal
        self._dispatcher = dispatcher
        self.durability_barrier = durability_barrier
        self.after_apply_delay_seconds = after_apply_delay_seconds
        self.response_delay_seconds = response_delay_seconds

    def balance(self, player: str) -> int:
        return self._dispatcher.call(
            lambda: self._journal.balance(self._require_umoney(), player)
        )

    def transfer(self, operation: str, player: str, amount: int, idempotency_key: str) -> dict[str, Any]:
        balance = self._dispatcher.call(
            lambda: self._journal.apply(
                self._require_umoney(),
                operation,
                player,
                amount,
                idempotency_key,
                durability_barrier=self.durability_barrier,
                after_apply_delay_seconds=self.after_apply_delay_seconds,
            )
        )
        if self.response_delay_seconds > 0:
            time.sleep(self.response_delay_seconds)
        return {
            "ok": True,
            "applied": True,
            "idempotency_key": idempotency_key,
            "balance": balance,
        }

    def _require_umoney(self) -> Any:
        umoney = self._umoney_provider()
        required = ("api_get_player_money", "api_change_player_money")
        if umoney is None or any(not callable(getattr(umoney, name, None)) for name in required):
            raise BridgeOperationError(
                "the UMoney plugin is not loaded or has no compatible API",
                code="umoney_unavailable",
                status=503,
                retryable=True,
            )
        return umoney


def make_handler(state: BridgeState) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        server_version = "ExchangeUMoneyBridge/1.0"

        def do_GET(self) -> None:
            if not self._authorized():
                return
            parsed = urlparse(self.path)
            if parsed.path == "/health":
                self._json(200, {"ok": True})
                return
            if parsed.path != "/balance":
                self._json(404, {"ok": False, "code": "not_found", "error": "not found"})
                return
            player = parse_qs(parsed.query).get("player", [""])[0]
            if not player:
                self._json(
                    400,
                    {
                        "ok": False,
                        "code": "invalid_request",
                        "error": "player is required",
                        "retryable": False,
                        "applied": False,
                    },
                )
                return
            try:
                self._json(200, {"ok": True, "balance": state.balance(player)})
            except BridgeOperationError as error:
                self._operation_error(error)

        def do_POST(self) -> None:
            if not self._authorized():
                return
            parsed = urlparse(self.path)
            if parsed.path not in {"/debit", "/credit"}:
                self._json(404, {"ok": False, "code": "not_found", "error": "not found"})
                return
            try:
                payload = self._read_json()
                player = payload["player"]
                amount = payload["amount"]
                key = payload["idempotency_key"]
                if (
                    type(player) is not str
                    or not player
                    or len(player) > 64
                    or type(amount) is not int
                    or amount <= 0
                    or type(key) is not str
                    or not key
                    or len(key) > 128
                ):
                    raise BridgeOperationError(
                        "player, amount, and idempotency_key have invalid types or values",
                        code="invalid_request",
                        status=400,
                        retryable=False,
                    )
                operation = parsed.path.removeprefix("/")
                self._json(200, state.transfer(operation, player, amount, key))
            except KeyError as error:
                self._operation_error(
                    BridgeOperationError(
                        f"missing {error.args[0]}",
                        code="invalid_request",
                        status=400,
                        retryable=False,
                    )
                )
            except (TypeError, ValueError, json.JSONDecodeError):
                self._operation_error(
                    BridgeOperationError(
                        "request body must contain valid JSON values",
                        code="invalid_request",
                        status=400,
                        retryable=False,
                    )
                )
            except BridgeOperationError as error:
                self._operation_error(error)
            except Exception:
                self._operation_error(
                    BridgeOperationError(
                        "unexpected bridge error",
                        code="internal_error",
                        status=500,
                        retryable=True,
                    )
                )

        def log_message(self, _format: str, *_args: Any) -> None:
            return None

        def _authorized(self) -> bool:
            expected = f"Bearer {state.token}"
            supplied = self.headers.get("Authorization", "")
            if hmac.compare_digest(supplied, expected):
                return True
            self._json(
                401,
                {
                    "ok": False,
                    "code": "unauthorized",
                    "error": "unauthorized",
                    "retryable": False,
                    "applied": False,
                },
            )
            return False

        def _read_json(self) -> dict[str, Any]:
            length = int(self.headers.get("Content-Length") or "0")
            if length <= 0 or length > 65_536:
                raise ValueError("invalid content length")
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
            if type(payload) is not dict:
                raise ValueError("request body must be a JSON object")
            return payload

        def _operation_error(self, error: BridgeOperationError) -> None:
            self._json(
                error.status,
                {
                    "ok": False,
                    "code": error.code,
                    "error": str(error),
                    "retryable": error.retryable,
                    "applied": error.applied,
                },
            )

        def _json(self, status: int, payload: dict[str, Any]) -> None:
            body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)

    return Handler
