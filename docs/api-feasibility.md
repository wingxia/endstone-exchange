# Endstone v0.11.6 feasibility

This implementation is pinned to the latest verified stable release, Endstone v0.11.6 (published 2026-07-10).

| Requirement | Endstone v0.11.6 API | Implementation |
| --- | --- | --- |
| Right-click a block | `PlayerInteractEvent` and `Action::RightClickBlock` | Public API |
| Right-click an actor | `PlayerInteractActorEvent` | Public API |
| Detect a stick named `exchanger` | `ItemStack`, `ItemType`, and `ItemMeta::getDisplayName()` | Public API |
| Read/write player inventory | `PlayerInventory`, slot access, offhand access and `addItem` | Public API; exact identity is compared from type, data value and NBT instead of runtime `ItemMeta::isSimilar` state |
| Preserve custom item data | `ItemStack::getNbt()` / `setNbt()` | Public API plus a deterministic plugin-side codec for SQL storage |
| Trading UI | `ActionForm`, `Button`, `ModalForm`, and `TextInput` | Public API; actions are rebuilt from the current order book, and direct-trade input forms contain quantity only |
| Client language | `Player::getLocale()`, `ItemStack::getTranslationKey()`, and `Server::getLanguage()` | Public API; custom plugin messages use complete embedded catalogs for simplified Chinese, English, traditional Chinese and Japanese, while vanilla item names reuse Minecraft translations |
| Floating trade text | `Dimension::spawnActor`, `Dimension::getLoadedChunks`, `ChunkLoadEvent`, `PlayerMoveEvent`, `Actor::setNameTag*`, `Player::sendPacket`, scheduler | One protected armor-stand nameplate per loaded trade point; its metadata name is generated per recipient locale, with delayed appearance re-sync after chunk load, player movement and reconnect |
| Persistent targets | Block coordinates or persistent scoreboard tags on actors | Public API + MySQL |
| Read an item frame's displayed item | Not exposed by `Block`/`BlockState` | Public `Server::dispatchCommand()` saves a namespaced structure; after an asynchronous 80-tick delay the plugin reads that structure's BlockActor NBT from the world LevelDB log, then deletes the capture |

The public API can mark any clicked block or actor. A real exchange also has to deliver a real inventory item, so activation is accepted only when the target maps to an item: a block with an item form, an item-frame item, a dropped item, the actor's direct item form, or a matching spawn egg. Targets such as projectiles that have no deliverable item are rejected with an explanation.

Bedrock exposes buttons only in an action form and text inputs only in a custom modal form. It does not expose a native control that places adjustment buttons to the left and right of an input, and it cannot dynamically hide one control after a dropdown changes. The plugin therefore uses a stable three-step flow: choose an action, fill integer fields, then adjust or confirm. The action form omits direct purchase/sale buttons when the matching side is empty, and the direct-trade modal is constructed with exactly one field—the quantity—so a hidden or forged price cannot enter that path.

Vanilla Bedrock armor stands include normal physics and gravity. The plugin therefore places block labels directly on the target's top surface and does not teleport them during price refreshes. It also checks `Dimension::getLoadedChunks()` before spawning, waits 20 ticks for persisted actors to return, and reconciles duplicate tagged actors after `ChunkLoadEvent`. Because a raw metadata update is ignored when a reconnecting Bedrock client has not received that actor's `AddActor` packet, labels in chunks that were already loaded when the player joined are recreated once after spawn or the first chunk crossing; the replacement waits five ticks after removal and then retries appearance metadata. Newly loaded chunks use their normal `AddActor` flow without an unnecessary recreation. This avoids the former one-second vertical jump, the unloaded-chunk duplicate race, and missing labels after reconnect.

Item-frame contents are the one API gap in v0.11.6. Bedrock Dedicated Server stores a `structure save ... disk` result under a `structuretemplate_<namespace:name>` key in the world's LevelDB, rather than as a standalone `.mcstructure` file. The capture therefore runs asynchronously so LevelDB can flush the write without blocking the server thread. A second exchanger click while capture is pending cancels it. If the frame's `Item` tag is absent, the market item is `minecraft:frame`.

Primary references:

- [Endstone v0.11.6 release](https://github.com/EndstoneMC/endstone/releases/tag/v0.11.6)
- [C++ PlayerInteractEvent](https://endstone.dev/latest/reference/cpp/classendstone_1_1PlayerInteractEvent/)
- [C++ PlayerInteractActorEvent](https://endstone.dev/latest/reference/cpp/classendstone_1_1PlayerInteractActorEvent/)
- [C++ Actor API](https://endstone.dev/latest/reference/cpp/classendstone_1_1Actor/)
- [C++ Player locale API](https://endstone.dev/latest/reference/cpp/classendstone_1_1Player/#function-getlocale)
- [Endstone forms reference](https://endstone.dev/latest/reference/python/form/)
