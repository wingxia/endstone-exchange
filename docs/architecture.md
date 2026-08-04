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
| `trade_form` | Compact overview/review text, available-action selection, strict integer parsing and trade-action mapping | Endstone players, inventory mutation or database access |
| `localization` | Client-locale normalization, complete four-language message catalogs, safe formatting and player-error boundaries | Endstone events, SQL or inventory mutation |
| `exchange_service` | Market generations, price-time matching, escrow state machines and settlement invariants | Endstone objects |
| `umoney_gateway` | Bounded loopback HTTP client, off-main-thread requests and completion queue | Endstone objects or UMoney internals |
| `python/umoney_bridge` | Authenticated loopback service, Endstone-main-thread UMoney calls, SQLite idempotency and durability barrier | Exchange SQL or order matching |
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
- `exchange_economy_transfers` is the durable cross-system saga. A withdrawal reserves internal balance before UMoney credit; a deposit credits internal balance only after the UMoney debit is known applied. `operation_key` is immutable and unique.
- `exchange_frame_listings` stores immutable item snapshots and moves one-price item-frame sales through `ACTIVE`, `PAID`, `DROPPED`, and `CLAIMED`; cancellation is only legal before payment. `exchange_frame_listing_bindings` is the unique mutable guard for a frame until its marked drop is picked up.
- A frame purchase locks the listing and both accounts in one MySQL transaction, writes a balanced `FRAME_PURCHASE` / `FRAME_SALE` ledger pair, then revalidates and clears the physical frame. Structure capture re-saves and retries up to three times when the LevelDB record is not visible yet. Clearing recreates the frame from its original `BlockData` without physics, verifies the empty BlockActor, and only then spawns the drop; this avoids treating item frames as command containers. The dropped stack carries a hidden listing marker and unlimited lifetime so restart recovery can adopt an existing drop instead of duplicating it.
- The local UMoney bridge records `PREPARED` before calling UMoney and `COMPLETED` only after the UMoney money file passes an `fsync` durability barrier. A retry observes the expected before/after balance and never blindly applies the same delta twice.
- A network timeout remains retryable because the bridge idempotency key can safely reveal the result. A conflicting observed balance is moved to `BLOCKED` for manual reconciliation instead of guessing, refunding, or applying another delta.
- UMoney mode requires zero initial balance, disables administrative `addbalance`, and refuses startup when account balances, funded buy orders and pending withdrawals exceed the net externally applied UMoney deposits. This prevents legacy or operator-minted internal value from becoming withdrawable UMoney.
- Money is signed 64-bit integer cents. Player-entered prices are strict whole-`u` strings; the server validates the configured range and step before mapping them to exact cents.

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
| 8 | Crash-recoverable UMoney deposits and withdrawals with persisted external units |
| 9 | Public one-price item-frame listings with atomic payment and marked-drop recovery |

The embedded runner checks `exchange_schema_versions` and makes each upgrade retry-safe. Files under `migrations/` are the reviewable SQL equivalents; the plugin remains self-contained at deployment time.

## Runtime performance

- Periodic label reads are one asynchronous batch snapshot, not three queries per market on the server thread.
- Every form transition closes first and opens the next page one tick later. The action page is rebuilt from the current orders as one normal-size compact text block plus two to four single-line buttons. The input page contains quantity plus an optional limit price, and the review page is a two-button message form for confirm or edit. After settlement, the authoritative main inventory and offhand are resent one tick later so transient escrow markers cannot leave a stale client-held item.
- Label carriers remain normal protected actors on the server, while a small public `Player::sendPacket` metadata update makes them `0.01` scale with a zero client collision box and overrides the label text for each recipient's locale. Block labels rest on the block instead of being teleported every refresh. The loaded-chunk index prevents off-screen spawning, a 20-tick grace period lets persisted actors return before any replacement is created, and chunk reconciliation removes stale duplicates. For target chunks that were already loaded when a player joined, one five-tick remove/recreate sequence after spawn or chunk crossing guarantees Bedrock receives `AddActor` before the repeated appearance metadata; chunks loaded by that player keep their normal lifecycle.
- The Endstone thread only applies a completed snapshot to actors.
- Connection, read and write operations have bounded timeouts.
- UMoney HTTP runs on one C++ worker thread. The Python bridge queues only the two public UMoney API calls onto Endstone's main thread, while SQLite and MySQL retain independent recovery records across process restarts.
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
- Locale resolution, catalog completeness and all player-visible flows in `zh_CN`, `en_US`, `zh_TW` and `ja_JP`, including per-recipient hologram metadata.
- Server termination at every delivery and sell-escrow state transition, followed by reconnect recovery.
- UMoney deposit and withdrawal success/failure, insufficient balance, missing account, bridge outage, response timeout, retry, and manual-reconciliation quarantine.
- Process termination after UMoney persistence but before the bridge reply, and after bridge completion but before MySQL completion; each restart must settle exactly once.
- UMoney file, bridge SQLite journal, exchange ledger, account balance and transfer table reconciliation after every injected crash.
- Full inventory, non-stackable items, custom NBT, empty and filled item frames.
- Public `price_tag` listing, repricing, cancellation, non-owner rejection, OP override, left-click confirmation, insufficient balance, duplicate confirmation, competing buyers, explosion/break protection, marked drop pickup, and termination before/after frame clearing and drop creation.
- Database timeout/reconnect, server restart, target chunk unload/reload, player reconnect, duplicate-label cleanup, stable label position and actor removal.
- Ledger reconciliation after every scenario.
