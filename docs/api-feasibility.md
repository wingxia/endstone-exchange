# Endstone v0.11.6 feasibility

This implementation is pinned to the latest verified stable release, Endstone v0.11.6 (published 2026-07-10).

| Requirement | Endstone v0.11.6 API | Implementation |
| --- | --- | --- |
| Right-click a block | `PlayerInteractEvent` and `Action::RightClickBlock` | Public API |
| Right-click an actor | `PlayerInteractActorEvent` | Public API |
| Detect a stick named `exchanger` | `ItemStack`, `ItemType`, and `ItemMeta::getDisplayName()` | Public API |
| Read/write player inventory | `PlayerInventory`, slot access, offhand access and `addItem` | Public API; exact identity is compared from type, data value and NBT instead of runtime `ItemMeta::isSimilar` state |
| Preserve custom item data | `ItemStack::getNbt()` / `setNbt()` | Public API plus a deterministic plugin-side codec for SQL storage |
| Trading UI | `ModalForm`, `Header`, `Label`, `Divider`, `Dropdown`, and `Slider` | Public API |
| Floating trade text | `Dimension::spawnActor`, `Dimension::getLoadedChunks`, `ChunkLoadEvent`, `PlayerMoveEvent`, `Actor::setNameTag*`, `Player::sendPacket`, scheduler | One protected armor-stand nameplate per loaded trade point, plus delayed appearance re-sync after chunk load, player movement and reconnect |
| Persistent targets | Block coordinates or persistent scoreboard tags on actors | Public API + MySQL |
| Read an item frame's displayed item | Not exposed by `Block`/`BlockState` | Public `Server::dispatchCommand()` saves a namespaced structure; after an asynchronous 80-tick delay the plugin reads that structure's BlockActor NBT from the world LevelDB log, then deletes the capture |

The public API can mark any clicked block or actor. A real exchange also has to deliver a real inventory item, so activation is accepted only when the target maps to an item: a block with an item form, an item-frame item, a dropped item, the actor's direct item form, or a matching spawn egg. Targets such as projectiles that have no deliverable item are rejected with an explanation.

Vanilla Bedrock armor stands include normal physics and gravity. The plugin therefore places block labels directly on the target's top surface and does not teleport them during price refreshes. It also checks `Dimension::getLoadedChunks()` before spawning, waits 20 ticks for persisted actors to return, reconciles duplicate tagged actors after `ChunkLoadEvent`, and retries the small client appearance packet after reconnect. This avoids both the former one-second vertical jump and the unloaded-chunk duplicate race.

Item-frame contents are the one API gap in v0.11.6. Bedrock Dedicated Server stores a `structure save ... disk` result under a `structuretemplate_<namespace:name>` key in the world's LevelDB, rather than as a standalone `.mcstructure` file. The capture therefore runs asynchronously so LevelDB can flush the write without blocking the server thread. A second exchanger click while capture is pending cancels it. If the frame's `Item` tag is absent, the market item is `minecraft:frame`.

Primary references:

- [Endstone v0.11.6 release](https://github.com/EndstoneMC/endstone/releases/tag/v0.11.6)
- [C++ PlayerInteractEvent](https://endstone.dev/latest/reference/cpp/classendstone_1_1PlayerInteractEvent/)
- [C++ PlayerInteractActorEvent](https://endstone.dev/latest/reference/cpp/classendstone_1_1PlayerInteractActorEvent/)
- [C++ Actor API](https://endstone.dev/latest/reference/cpp/classendstone_1_1Actor/)
- [Endstone forms reference](https://endstone.dev/latest/reference/python/form/)
