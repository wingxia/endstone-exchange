import json
import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
PACK_ROOT = REPO_ROOT / "resource_packs" / "exchange_chest_ui"


class ExchangeResourcePackTest(unittest.TestCase):
    def test_manifest_has_resource_module_identity(self):
        manifest = json.loads((PACK_ROOT / "manifest.json").read_text(encoding="utf-8"))

        self.assertEqual(manifest["format_version"], 2)
        self.assertEqual(manifest["header"]["name"], "Exchange Chest UI")
        self.assertRegex(manifest["header"]["uuid"], r"^[0-9a-f-]{36}$")
        self.assertEqual(manifest["header"]["version"], [0, 1, 1])
        self.assertEqual(manifest["header"]["min_engine_version"], [1, 21, 0])
        self.assertEqual(len(manifest["modules"]), 1)
        self.assertEqual(manifest["modules"][0]["type"], "resources")
        self.assertRegex(manifest["modules"][0]["uuid"], r"^[0-9a-f-]{36}$")
        self.assertNotEqual(manifest["header"]["uuid"], manifest["modules"][0]["uuid"])

    def test_ui_defs_registers_server_form_override(self):
        ui_defs = json.loads((PACK_ROOT / "ui" / "_ui_defs.json").read_text(encoding="utf-8"))

        self.assertEqual(ui_defs["ui_defs"], ["ui/server_form.json"])

    def test_server_form_uses_fixed_exchange_button_indices(self):
        server_form_path = PACK_ROOT / "ui" / "server_form.json"
        server_form = json.loads(server_form_path.read_text(encoding="utf-8"))
        raw = server_form_path.read_text(encoding="utf-8")

        self.assertEqual(server_form["namespace"], "server_form")
        self.assertIn("exchange_form_panel", server_form)
        self.assertIn("exchange_product_slots", server_form)
        self.assertIn("exchange_trade_slots", server_form)
        self.assertIn("exchange_hit_button@common_buttons.light_text_button", server_form)
        self.assertIn("exchange_button_icon", server_form)
        self.assertIn("exchange_button_label", server_form)
        self.assertEqual(server_form["long_form@common_dialogs.main_panel_no_buttons"]["$child_control"], "server_form.exchange_form_panel")

        indices = sorted({int(match) for match in re.findall(r'"collection_index"\s*:\s*(\d+)', raw)})
        self.assertEqual(indices, list(range(49)))
        self.assertEqual(server_form["exchange_hit_button@common_buttons.light_text_button"]["$button_text"], "")
        self.assertIn("#slot_text", raw)
        self.assertIn("binding_type\": \"collection_details", raw)
        self.assertIn("#form_button_texture", raw)
        self.assertIn("(not ((#slot_text = '') and (#slot_texture = '')))", raw)

        product_positions = re.findall(r'"grid_position"\s*:\s*\[(\d+),\s*(\d+)\].*?"collection_index"\s*:\s*(\d+)', raw)
        product_indices = sorted(int(index) for _, _, index in product_positions if 9 <= int(index) <= 36)
        self.assertEqual(product_indices, list(range(9, 37)))


if __name__ == "__main__":
    unittest.main()
