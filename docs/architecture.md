# Architecture and evolution plan

## Module boundaries

| Module | Responsibility | Must not own |
| --- | --- | --- |
| `plugin` | Endstone events, commands, player messages, world targets and hologram actors | SQL schema or matching rules |
| `inventory_escrow` | Hidden inventory markers, crash recovery receipts, NBT-safe delivery cleanup | Order matching or account balances |
| `price_window` | Exact integer-cent mapping for Bedrock float sliders | Form rendering or database access |
| `exchange_service` | Market generations, price-time matching, escrow state machines and settlement invariants | Endstone objects |
| `database` | MySQL connection, timeouts, transactions and ordered schema upgrades | Gameplay decisions |
| `nbt_codec` | Deterministic item NBT encoding | World storage discovery |
| `structure_reader` | Item-frame structure capture compatibility layer | General exchange logic |

The Endstone adapter may call the domain service, but the service never calls Endstone. This keeps matching and database recovery testable without starting BDS.

## Durable identities and invariants

- `exchange_markets` is immutable. Reopening the same block or actor creates a new market generation.
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

The embedded runner checks `exchange_schema_versions` and makes each upgrade retry-safe. Files under `migrations/` are the reviewable SQL equivalents; the plugin remains self-contained at deployment time.

## Runtime performance

- Periodic hologram reads are one asynchronous batch snapshot, not three queries per market on the server thread.
- The Endstone thread only applies a completed snapshot to actors.
- Connection, read and write operations have bounded timeouts.
- Matching transactions lock only the relevant market, account and open-order rows.

## Planned extension seams

1. Split `exchange_service` into account, market, matching and settlement repositories once a second asset class or fee model is introduced.
2. Move all user-triggered database work onto a persistent single-writer worker with main-thread completion callbacks. Periodic holograms already use this pattern; order submission remains synchronous but timeout-bounded for the first release.
3. Add fee, tax and operator adjustments as new ledger reasons rather than mutable side columns.
4. Add an actor/chunk reconciliation index so missing actor targets can be quarantined instead of remaining active indefinitely.
5. Replace the structure/LevelDB item-frame adapter as soon as Endstone exposes a public item-frame inventory API.
6. Add metrics for query latency, open orders, unresolved claims/escrows, ledger mismatches and hologram snapshot age.

## Required release tests

- Fresh schema and upgrade from every supported schema version.
- Price-time priority, partial fills, cancellation, market close and self-trade exclusion.
- Server termination at every delivery and sell-escrow state transition, followed by reconnect recovery.
- Full inventory, non-stackable items, custom NBT, empty and filled item frames.
- Database timeout/reconnect, server restart, target chunk unload/reload and actor removal.
- Ledger reconciliation after every scenario.
