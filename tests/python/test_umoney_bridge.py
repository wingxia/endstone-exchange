from __future__ import annotations

import json
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python" / "umoney_bridge"))

from endstone_exchange_umoney_bridge.server import (  # noqa: E402
    BridgeJournal,
    BridgeOperationError,
    BridgeState,
    DirectDispatcher,
    make_handler,
)


class FakeUMoney:
    def __init__(self) -> None:
        self.money = {"Alice": 100, "Bob": 25}
        self.changes: list[tuple[str, int]] = []

    def api_get_player_money(self, player: str):
        return self.money.get(player)

    def api_change_player_money(self, player: str, change: int) -> None:
        if player not in self.money:
            return
        self.money[player] += change
        self.changes.append((player, change))


class JournalTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.path = Path(self.temp_dir.name) / "journal.sqlite3"
        self.umoney = FakeUMoney()
        self.journal = BridgeJournal(self.path)

    def tearDown(self) -> None:
        self.journal.close()
        self.temp_dir.cleanup()

    def test_debit_and_credit_are_idempotent(self) -> None:
        self.assertEqual(self.journal.apply(self.umoney, "debit", "Alice", 30, "deposit-1"), 70)
        self.assertEqual(self.journal.apply(self.umoney, "debit", "Alice", 30, "deposit-1"), 70)
        self.assertEqual(self.journal.apply(self.umoney, "credit", "Alice", 15, "withdraw-1"), 85)
        self.assertEqual(self.journal.apply(self.umoney, "credit", "Alice", 15, "withdraw-1"), 85)
        self.assertEqual(self.umoney.changes, [("Alice", -30), ("Alice", 15)])

    def test_prepared_operation_recovers_after_external_apply(self) -> None:
        self.journal._connection.execute(
            """
            INSERT INTO operations(
                idempotency_key,operation,player,amount,state,balance_before,balance_after
            ) VALUES (?,?,?,?,?,?,?)
            """,
            ("crash-1", "debit", "Alice", 40, "PREPARED", 100, 60),
        )
        self.journal._connection.commit()
        self.umoney.api_change_player_money("Alice", -40)
        self.journal.close()

        self.journal = BridgeJournal(self.path)
        self.assertEqual(self.journal.apply(self.umoney, "debit", "Alice", 40, "crash-1"), 60)
        self.assertEqual(self.umoney.changes, [("Alice", -40)])

    def test_recovery_conflict_requires_manual_reconciliation(self) -> None:
        self.journal._connection.execute(
            """
            INSERT INTO operations(
                idempotency_key,operation,player,amount,state,balance_before,balance_after
            ) VALUES (?,?,?,?,?,?,?)
            """,
            ("conflict-1", "debit", "Alice", 40, "PREPARED", 100, 60),
        )
        self.journal._connection.commit()
        self.umoney.money["Alice"] = 75
        with self.assertRaises(BridgeOperationError) as context:
            self.journal.apply(self.umoney, "debit", "Alice", 40, "conflict-1")
        self.assertEqual(context.exception.code, "manual_reconciliation_required")
        self.assertFalse(context.exception.retryable)

    def test_rejects_missing_player_and_insufficient_funds(self) -> None:
        with self.assertRaises(BridgeOperationError) as missing:
            self.journal.apply(self.umoney, "credit", "Missing", 5, "missing-1")
        self.assertEqual(missing.exception.code, "player_not_found")
        with self.assertRaises(BridgeOperationError) as insufficient:
            self.journal.apply(self.umoney, "debit", "Bob", 30, "insufficient-1")
        self.assertEqual(insufficient.exception.code, "insufficient_funds")

    def test_rejects_idempotency_key_reuse_with_different_request(self) -> None:
        self.journal.apply(self.umoney, "debit", "Alice", 10, "same-key")
        with self.assertRaises(BridgeOperationError) as context:
            self.journal.apply(self.umoney, "debit", "Alice", 11, "same-key")
        self.assertEqual(context.exception.code, "idempotency_conflict")

    def test_durability_barrier_runs_before_completion_and_is_retryable(self) -> None:
        attempts = 0

        def barrier() -> None:
            nonlocal attempts
            attempts += 1
            if attempts == 1:
                raise OSError("simulated fsync failure")

        with self.assertRaises(BridgeOperationError) as context:
            self.journal.apply(
                self.umoney,
                "debit",
                "Alice",
                10,
                "durability-1",
                durability_barrier=barrier,
            )
        self.assertEqual(context.exception.code, "durability_barrier_failed")
        self.assertTrue(context.exception.retryable)
        self.assertEqual(self.umoney.money["Alice"], 90)

        self.assertEqual(
            self.journal.apply(
                self.umoney,
                "debit",
                "Alice",
                10,
                "durability-1",
                durability_barrier=barrier,
            ),
            90,
        )
        self.assertEqual(attempts, 2)
        self.assertEqual(self.umoney.changes, [("Alice", -10)])


class HttpTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.umoney = FakeUMoney()
        self.journal = BridgeJournal(Path(self.temp_dir.name) / "journal.sqlite3")
        self.state = BridgeState(
            "test-secret-token-that-is-long-enough",
            lambda: self.umoney,
            self.journal,
            DirectDispatcher(),
        )
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(self.state))
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.journal.close()
        self.temp_dir.cleanup()

    def request(self, method: str, path: str, payload=None, authorized: bool = True):
        body = None if payload is None else json.dumps(payload).encode("utf-8")
        request = urllib.request.Request(
            f"http://127.0.0.1:{self.server.server_port}{path}",
            data=body,
            method=method,
        )
        if authorized:
            request.add_header("Authorization", "Bearer test-secret-token-that-is-long-enough")
        if body is not None:
            request.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(request, timeout=2) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as error:
            with error:
                return error.code, json.loads(error.read())

    def test_http_auth_balance_and_idempotent_transfer(self) -> None:
        self.assertEqual(self.request("GET", "/health", authorized=False)[0], 401)
        self.assertEqual(self.request("GET", "/balance?player=Alice")[1]["balance"], 100)
        payload = {"player": "Alice", "amount": 20, "idempotency_key": "http-1"}
        self.assertEqual(self.request("POST", "/debit", payload)[1]["balance"], 80)
        self.assertEqual(self.request("POST", "/debit", payload)[1]["balance"], 80)
        self.assertEqual(self.umoney.changes, [("Alice", -20)])

    def test_http_rejects_coerced_financial_values(self) -> None:
        invalid_payloads = [
            {"player": "Alice", "amount": True, "idempotency_key": "bad-bool"},
            {"player": "Alice", "amount": 1.5, "idempotency_key": "bad-float"},
            {"player": "Alice", "amount": "10", "idempotency_key": "bad-string"},
            {"player": 123, "amount": 10, "idempotency_key": "bad-player"},
            {"player": "Alice", "amount": 10, "idempotency_key": 123},
        ]
        for payload in invalid_payloads:
            with self.subTest(payload=payload):
                status, response = self.request("POST", "/debit", payload)
                self.assertEqual(status, 400)
                self.assertEqual(response["code"], "invalid_request")
        self.assertEqual(self.umoney.money["Alice"], 100)
        self.assertEqual(self.umoney.changes, [])

    def test_rejects_non_integer_umoney_balance(self) -> None:
        self.umoney.money["Alice"] = 100.5
        status, response = self.request("GET", "/balance?player=Alice")
        self.assertEqual(status, 502)
        self.assertEqual(response["code"], "invalid_balance")


if __name__ == "__main__":
    unittest.main()
