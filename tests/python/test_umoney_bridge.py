import json
import sys
import unittest
from http.client import HTTPConnection
from http.server import ThreadingHTTPServer
from pathlib import Path
from threading import Thread


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python" / "umoney_bridge"))

from endstone_exchange_umoney_bridge.server import BridgeState, make_handler  # noqa: E402


class FakeUMoney:
    def __init__(self):
        self.money = {"Steve": 100}

    def api_get_player_money(self, player):
        return self.money.get(player, 0)

    def api_change_player_money(self, player, change):
        self.money[player] = self.money.get(player, 0) + change


class BridgeTest(unittest.TestCase):
    def setUp(self):
        self.umoney = FakeUMoney()
        state = BridgeState("secret", self.umoney)
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(state))
        self.thread = Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_address[1]

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)

    def request(self, method, path, body=None, token="secret"):
        conn = HTTPConnection("127.0.0.1", self.port, timeout=5)
        headers = {"Authorization": f"Bearer {token}"}
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        conn.request(method, path, data, headers)
        resp = conn.getresponse()
        payload = json.loads(resp.read().decode())
        conn.close()
        return resp.status, payload

    def test_balance_and_debit_are_idempotent(self):
        status, payload = self.request("GET", "/balance?player=Steve")
        self.assertEqual(status, 200)
        self.assertEqual(payload["balance"], 100)

        body = {"player": "Steve", "amount": 30, "idempotency_key": "k1"}
        self.assertEqual(self.request("POST", "/debit", body)[1]["balance"], 70)
        self.assertEqual(self.request("POST", "/debit", body)[1]["balance"], 70)
        self.assertEqual(self.umoney.money["Steve"], 70)

    def test_rejects_bad_token_and_insufficient_funds(self):
        self.assertEqual(self.request("GET", "/balance?player=Steve", token="bad")[0], 401)
        status, payload = self.request("POST", "/debit", {"player": "Steve", "amount": 200, "idempotency_key": "k2"})
        self.assertEqual(status, 400)
        self.assertIn("insufficient", payload["error"])


if __name__ == "__main__":
    unittest.main()

