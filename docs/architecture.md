# Architecture and evolution plan

## Module boundaries

| Module | Responsibility | Must not own |
| --- | --- | --- |
| `plugin` | Endstone events, commands, player messages, world targets and hologram actors | SQL schema or matching rules |
| `inventory_escrow` | Hidden inventory markers, crash recovery receipts, NBT-safe delivery cleanup | Order matching or account balances |
| `hologram_packet` | Version-pinned Bedrock `SetActorData` payload for a tiny, zero-box nameplate carrier | Markets, SQL or world lookup |
| `market_display` | Stable label anchors, chunk coordinate mapping and simple player-facing trade text | Endstone actors, SQL or scheduling |
| `item_identity` | Canonical type, data value and complete-NBT comparison | Inventory mutation or SQL access |
| `interaction_gate` | Per-player duplicate right-click suppression | Gameplay or database state |
| `price_window` | Exact integer-cent mapping for Bedrock float sliders | Form rendering or database access |
| `exchange_service` | Market generations, price-time matching, escrow state machines and settlement invariants | Endstone objects |
| `database` | MySQL connection, timeouts, transactions and ordered schema upgrades | Gameplay decisions |
| `nbt_codec` | Deterministic item NBT encoding | World storage discovery |
| `structure_reader` | Item-frame structure capture compatibility layer | General exchange logic |

The Endstone adapter may call the domain service, but the service never calls Endstone. This keeps matching and database recovery testable without starting BDS.

## Durable identities and invariants

- `exchange_books` is the durable product identity keyed by type, data value and complete NBT. All physical markets for the same exact item share its order book and matching liquidity.
- `exchange_markets` keeps immutable item snapshots and a physical world target. A manual close marks one generation reopenable without touching its orders; reopening the same target and item resumes that generation, while another position remains independently switchable.
- `exchange_target_bindings` is the only mutable pointer from a world target to its active generation.
- Orders, trades and deliveries always reference an immutable market item snapshot.
- `exchange_delivery_claims` reserves a delivery before a claim marker enters the inventory. Reconnect either applies the tagged quantity or releases the reservation.
- `exchange_sell_escrows` moves whole matching stacks through `PREPARED`, `TAGGED`, `ORDERED` and `CLEANED`. Temporary barrier receipts make an interrupted inventory removal distinguishable from an order that was never funded.
- `exchange_balance_ledger` is append-only. For every player, `SUM(delta_cents)` must equal `exchange_accounts.balance_cents`.
- Money is signed 64-bit integer cents. Bedrock sliders submit only small integer indexes; the server maps them back to exact cents.

## Schema upgrades

| Version | Purpose |
| --- | --- |
| 1 | Accounts, markets, orders, trades and deliveries |
| 2 | Immutable market generations and active target bindings |
| 3 | Two-phase, idempotent delivery claims |
| 4 | Crash-recoverable sell inventory escrow |
| 5 | Immutable balance ledger and opening-balance backfill |
| 6 | Paused markets whose funded order books survive a manual close and resume safely |
| 7 | Shared item books: independent physical locations with one exact-item order book |

The embedded runner checks `exchange_schema_versions` and makes each upgrade retry-safe. Files under `migrations/` are the reviewable SQL equivalents; the plugin remains self-contained at deployment time.

## Runtime performance

- Periodic label reads are one asynchronous batch snapshot, not three queries per market on the server thread.
- A form submission closes first and executes one tick later. After settlement, the authoritative main inventory and offhand are resent one tick later so transient escrow markers cannot leave a stale client-held item.
- Label carriers remain normal protected actors on the server, while a small public `Player::sendPacket` metadata update makes them `0.01` scale with a zero client collision box. Block labels rest on the block instead of being teleported every refresh. The loaded-chunk index prevents off-screen spawning, a 20-tick grace period lets persisted actors return before any replacement is created, and chunk reconciliation removes stale duplicates. For target chunks that were already loaded when a player joined, one five-tick remove/recreate sequence after spawn or chunk crossing guarantees Bedrock receives `AddActor` before the repeated appearance metadata; chunks loaded by that player keep their normal lifecycle.
- The Endstone thread only applies a completed snapshot to actors.
- Connection, read and write operations have bounded timeouts.
- Matching transactions lock only the submitted physical market, account and open-order rows for its shared item book.

## Planned extension seams

1. Split `exchange_service` into account, market, matching and settlement repositories once a second asset class or fee model is introduced.
2. Move all user-triggered database work onto a persistent single-writer worker with main-thread completion callbacks. Periodic holograms already use this pattern; order submission remains synchronous but timeout-bounded for the first release.
3. Add fee, tax and operator adjustments as new ledger reasons rather than mutable side columns.
4. Add operator diagnostics for quarantining a missing actor target after repeated loaded-chunk reconciliation failures.
5. Replace the structure/LevelDB item-frame adapter as soon as Endstone exposes a public item-frame inventory API.
6. Add metrics for query latency, open orders, unresolved claims/escrows, ledger mismatches and hologram snapshot age.

## Required release tests

- Fresh schema and upgrade from every supported schema version.
- Price-time priority, partial fills, crossed limit prices, cancellation, market close and self-trade exclusion.
- Same-item cross-position matching, independent position toggles and different-NBT isolation.
- Repeated interaction packets, duplicate forms, form close after submission, main inventory and offhand sell escrow.
- Server termination at every delivery and sell-escrow state transition, followed by reconnect recovery.
- Full inventory, non-stackable items, custom NBT, empty and filled item frames.
- Database timeout/reconnect, server restart, target chunk unload/reload, player reconnect, duplicate-label cleanup, stable label position and actor removal.
- Ledger reconciliation after every scenario.
