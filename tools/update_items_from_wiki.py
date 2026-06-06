#!/usr/bin/env python3
"""Generate the Exchange item catalog from Minecraft Wiki Bedrock tables."""

from __future__ import annotations

import html
import json
import re
import subprocess
import sys
import textwrap
import urllib.request
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ITEMS_URL = "https://minecraft.wiki/w/Bedrock_Edition_data_values/Items2?action=raw"
BLOCKS_URL = "https://minecraft.wiki/w/Bedrock_Edition_data_values/Blocks2?action=raw"
MIN_CATALOG_ENTRIES = 1866
VALID_ICON_PREFIXES = ("textures/items/", "textures/blocks/")

CATEGORY_DEFS = [
    ("common", "常用", "textures/items/emerald"),
    ("building", "建筑方块", "textures/blocks/stone"),
    ("ores", "矿物材料", "textures/items/diamond"),
    ("redstone", "红石机械", "textures/items/redstone_dust"),
    ("food", "食物作物", "textures/items/bread"),
    ("tools", "工具武器", "textures/items/diamond_pickaxe"),
    ("armor", "防具装备", "textures/items/diamond_chestplate"),
    ("nature", "自然装饰", "textures/blocks/oak_sapling"),
    ("spawn_eggs", "刷怪蛋", "textures/items/egg"),
    ("misc", "其他物品", "textures/items/paper"),
]

COMMON_IDS = {
    "minecraft:stone",
    "minecraft:cobblestone",
    "minecraft:dirt",
    "minecraft:oak_log",
    "minecraft:oak_planks",
    "minecraft:diamond",
    "minecraft:iron_ingot",
    "minecraft:gold_ingot",
    "minecraft:emerald",
    "minecraft:redstone",
    "minecraft:coal",
    "minecraft:bread",
}

UNTRADABLE_IDS = {
    "minecraft:air",
    "minecraft:flowing_water",
    "minecraft:water",
    "minecraft:flowing_lava",
    "minecraft:lava",
    "minecraft:fire",
    "minecraft:portal",
    "minecraft:end_portal",
    "minecraft:reserved6",
    "minecraft:unknown",
    "minecraft:info_update",
    "minecraft:info_update2",
    "minecraft:movingblock",
}


@dataclass(frozen=True)
class Entry:
    item_id: str
    name: str
    category: str
    icon: str
    source: str


def fetch(url: str) -> str:
    request = urllib.request.Request(url, headers={"User-Agent": "EndstoneExchangeCatalog/1.0"})
    try:
        with urllib.request.urlopen(request, timeout=45) as response:
            return response.read().decode("utf-8")
    except Exception as exc:
        print(f"urllib fetch failed for {url}: {exc}; retrying with curl --http1.1", file=sys.stderr)

    result = subprocess.run(
        [
            "curl",
            "--http1.1",
            "-L",
            "--fail",
            "--silent",
            "--show-error",
            "--max-time",
            "60",
            "-A",
            "EndstoneExchangeCatalog/1.0",
            url,
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout


def split_template_args(payload: str) -> list[str]:
    return [part.strip() for part in payload.split("|") if part.strip()]


def parse_keyed_args(parts: list[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    for part in parts:
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        out[key.strip()] = value.strip()
    return out


def first_template_name(parts: list[str]) -> str | None:
    for part in parts:
        if "=" in part:
            continue
        if part in {"S", "N", "E", "I", "SE", "ISE", "foot", "bedrock"}:
            continue
        return clean_wiki_text(part)
    return None


def clean_wiki_text(value: str) -> str:
    value = value.strip()
    value = re.sub(r"<[^>]+>", "", value)
    value = re.sub(r"\{\{code\|([^}]+)\}\}", r"\1", value)
    value = re.sub(r"\[\[([^|\]]+)\|([^]]+)\]\]", r"\2", value)
    value = re.sub(r"\[\[([^]]+)\]\]", r"\1", value)
    return html.unescape(value).strip()


def texture_key(value: str) -> str:
    value = clean_wiki_text(value)
    value = re.sub(r"\.(png|gif)$", "", value, flags=re.IGNORECASE)
    value = value.lower().replace(" ", "_").replace("-", "_")
    value = re.sub(r"[^a-z0-9_]", "", value)
    return value


def parse_items(raw: str) -> list[tuple[str, str, str]]:
    entries: list[tuple[str, str, str]] = []
    for match in re.finditer(r"\{\{Data table\|([^{}]+)\}\}", raw):
        parts = split_template_args(match.group(1))
        args = parse_keyed_args(parts)
        item_id = args.get("nameid")
        if not item_id:
            continue
        name = first_template_name(parts) or item_id.replace("_", " ").title()
        icon = texture_key(args.get("icon", item_id)) or texture_key(item_id)
        entries.append((f"minecraft:{item_id}", name, f"textures/items/{icon}"))
    return entries


def parse_blocks(raw: str) -> list[tuple[str, str, str]]:
    entries: list[tuple[str, str, str]] = []
    row_re = re.compile(
        r"\[\[File:BlockSprite ([^\]]+?)\.png\]\]\|\|[^|]*\|\|[^|]*\|\|"
        r"\{\{code\|([^}]+)\}\}\|\|\[\[([^\]]+)\]\]",
        re.IGNORECASE,
    )
    for match in row_re.finditer(raw):
        icon_name, resource_location, display = match.groups()
        item_id = f"minecraft:{resource_location.strip()}"
        name = clean_wiki_text(f"[[{display}]]")
        icon = f"textures/blocks/{texture_key(icon_name) or texture_key(resource_location)}"
        entries.append((item_id, name, icon))
    return entries


def classify(item_id: str, name: str, source: str) -> str:
    key = item_id.removeprefix("minecraft:")
    text = f"{key} {name}".lower()
    if item_id in COMMON_IDS:
        return "common"
    if key.endswith("_spawn_egg"):
        return "spawn_eggs"
    if any(token in text for token in ["helmet", "chestplate", "leggings", "boots", "elytra", "horse_armor", "wolf_armor", "shield"]):
        return "armor"
    if any(token in text for token in ["sword", "pickaxe", "axe", "shovel", "hoe", "bow", "crossbow", "trident", "mace", "shears", "fishing_rod", "flint_and_steel", "brush"]):
        return "tools"
    if any(token in text for token in ["apple", "bread", "beef", "porkchop", "chicken", "rabbit", "mutton", "cod", "salmon", "fish", "stew", "soup", "cookie", "carrot", "potato", "beetroot", "berries", "melon", "pumpkin_pie", "kelp", "seeds", "wheat", "wart", "pod"]):
        return "food"
    if any(token in text for token in ["redstone", "piston", "rail", "hopper", "dispenser", "dropper", "observer", "repeater", "comparator", "lever", "button", "pressure_plate", "daylight", "target", "tripwire"]):
        return "redstone"
    if any(token in text for token in ["ore", "ingot", "nugget", "diamond", "emerald", "lapis", "quartz", "coal", "copper", "netherite", "amethyst", "raw_iron", "raw_gold", "raw_copper"]):
        return "ores"
    if source == "block":
        if any(token in text for token in ["sapling", "leaves", "flower", "mushroom", "coral", "grass", "bamboo", "vine", "roots", "fungus", "plant", "cactus", "kelp"]):
            return "nature"
        return "building"
    return "misc"


def search_terms(item_id: str, name: str, category: str) -> list[str]:
    raw_terms = [item_id, item_id.removeprefix("minecraft:"), name, category]
    raw_terms.extend(re.split(r"[\s_:()'/-]+", item_id.removeprefix("minecraft:")))
    raw_terms.extend(re.split(r"[\s_:()'/-]+", name.lower()))
    terms: list[str] = []
    seen: set[str] = set()
    for term in raw_terms:
        term = term.strip().lower()
        if not term or term in seen:
            continue
        seen.add(term)
        terms.append(term)
    return terms


def build_entries(items_raw: str, blocks_raw: str) -> list[Entry]:
    merged: dict[str, Entry] = {}
    for source, parsed in (("block", parse_blocks(blocks_raw)), ("item", parse_items(items_raw))):
        for item_id, name, icon in parsed:
            if item_id in UNTRADABLE_IDS:
                continue
            if item_id in merged:
                continue
            category = classify(item_id, name, source)
            merged[item_id] = Entry(item_id=item_id, name=name, category=category, icon=icon, source=source)
    return sorted(merged.values(), key=lambda e: (category_index(e.category), e.name.lower(), e.item_id))


def validate_entries(entries: list[Entry]) -> None:
    errors: list[str] = []
    category_ids = {cat_id for cat_id, _name, _icon in CATEGORY_DEFS}
    if len(entries) < MIN_CATALOG_ENTRIES:
        errors.append(f"catalog has {len(entries)} entries, expected at least {MIN_CATALOG_ENTRIES}")
    seen: set[str] = set()
    for entry in entries:
        if not entry.item_id.startswith("minecraft:"):
            errors.append(f"{entry.item_id}: item_id must start with minecraft:")
        if entry.item_id in seen:
            errors.append(f"{entry.item_id}: duplicate item_id")
        seen.add(entry.item_id)
        if not entry.name.strip():
            errors.append(f"{entry.item_id}: missing display name")
        if entry.category not in category_ids:
            errors.append(f"{entry.item_id}: unknown category {entry.category}")
        if not entry.icon.strip():
            errors.append(f"{entry.item_id}: missing icon")
        elif not entry.icon.startswith(VALID_ICON_PREFIXES):
            errors.append(f"{entry.item_id}: icon must use textures/items or textures/blocks: {entry.icon}")
        elif entry.icon in VALID_ICON_PREFIXES:
            errors.append(f"{entry.item_id}: icon path is incomplete")
    if errors:
        raise SystemExit("Catalog validation failed:\n" + "\n".join(f"- {error}" for error in errors[:25]))


def category_index(category: str) -> int:
    for index, (cat_id, _name, _icon) in enumerate(CATEGORY_DEFS):
        if cat_id == category:
            return index
    return len(CATEGORY_DEFS)


def cpp_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def emit_cpp(entries: list[Entry]) -> str:
    lines = [
        '#include "endstone_exchange/catalog/GeneratedCatalog.hpp"',
        "",
        '#include "endstone_exchange/core/ItemIdentity.hpp"',
        "",
        "namespace exchange::catalog {",
        "",
        "const std::vector<CatalogCategory>& categories() {",
        "    static const std::vector<CatalogCategory> value = {",
    ]
    for cat_id, name, icon in CATEGORY_DEFS:
        lines.append(f"        {{{cpp_string(cat_id)}, {cpp_string(name)}, {cpp_string(icon)}}},")
    lines.extend([
        "    };",
        "    return value;",
        "}",
        "",
        "const std::vector<Product>& products() {",
        "    static const std::vector<Product> value = {",
    ])
    for entry in entries:
        terms = ", ".join(cpp_string(term) for term in search_terms(entry.item_id, entry.name, entry.category))
        lines.append(
            "        Product{"
            f"productKey({cpp_string(entry.item_id)}, {{}}), "
            f"{cpp_string(entry.item_id)}, {{}}, {cpp_string(entry.name)}, "
            f"{cpp_string(entry.category)}, {cpp_string(entry.icon)}, true, "
            f"{{{terms}}}"
            "},"
        )
    lines.extend([
        "    };",
        "    return value;",
        "}",
        "",
        "}  // namespace exchange::catalog",
        "",
    ])
    return "\n".join(lines)


def emit_yaml(entries: list[Entry]) -> str:
    lines = [
        "# Generated by tools/update_items_from_wiki.py.",
        f"# Sources: {BLOCKS_URL}",
        f"#          {ITEMS_URL}",
        "categories:",
    ]
    by_category: dict[str, list[Entry]] = {}
    for entry in entries:
        by_category.setdefault(entry.category, []).append(entry)
    for cat_id, name, icon in CATEGORY_DEFS:
        lines.append(f"  {cat_id}:")
        lines.append(f"    name: {name}")
        lines.append(f"    icon: {icon}")
        lines.append("    items:")
        for entry in by_category.get(cat_id, []):
            lines.append(f"      {entry.item_id}:")
            lines.append(f"        name: {entry.name}")
            lines.append(f"        icon: {entry.icon}")
            lines.append(f"        source: {entry.source}")
    return "\n".join(lines) + "\n"


def main() -> int:
    items_raw = fetch(ITEMS_URL)
    blocks_raw = fetch(BLOCKS_URL)
    entries = build_entries(items_raw, blocks_raw)
    validate_entries(entries)

    (ROOT / "cpp/src/catalog").mkdir(parents=True, exist_ok=True)
    (ROOT / "cpp/src/catalog/GeneratedCatalog.cpp").write_text(emit_cpp(entries), encoding="utf-8")
    (ROOT / "resources/items.yml").write_text(emit_yaml(entries), encoding="utf-8")
    print(textwrap.dedent(
        f"""\
        Generated {len(entries)} products from Minecraft Wiki:
          blocks: {BLOCKS_URL}
          items:  {ITEMS_URL}
        """
    ).strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
