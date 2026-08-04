# Isolated Endstone/BDS E2E drivers

These helpers exercise Exchange through a real Bedrock client and Endstone's
player/event paths. They are test-only components and must never remain installed
on a production server.

## Components

- `umoney_bds_e2e.js` connects with `bedrock-protocol`, keeps a deterministic
  offline identity per test username, records forms and command output, and can
  submit scripted form responses.
- `endstone_driver/` builds a temporary Python plugin whose `exchangee2e`
  command dispatches a small allowlist of Exchange commands as an online player.
- `endstone_event_driver.cpp` builds a temporary C++ plugin whose
  `exchangeevente2e` command raises a real left/right `PlayerInteractEvent` for
  a selected block and can supply synthetic `exchanger` or `price_tag` sticks.

The drivers assume an isolated offline-mode server, a disposable database, and a
dedicated `screen` session. Back up the test server before fault injection.

## Build

```sh
python3 -m pip wheel --no-deps ./tests/e2e/endstone_driver --wheel-dir build/e2e-dist
cmake -S . -B build-e2e -DEXCHANGE_BUILD_TESTS=ON -DEXCHANGE_BUILD_E2E_DRIVER=ON
cmake --build build-e2e --parallel
```

Install the resulting driver wheel and `endstone_exchange_event_e2e.so` only in
the disposable server. The normal release build leaves
`EXCHANGE_BUILD_E2E_DRIVER=OFF`.

## Bedrock client

Install `bedrock-protocol` in a temporary Node.js directory outside this
repository, then run:

```sh
E2E_HOST=127.0.0.1 \
E2E_PORT=19141 \
E2E_USERNAME=UMoneySmoke \
E2E_SCREEN_SESSION=exchange-test-19141 \
node tests/e2e/umoney_bds_e2e.js
```

Override `E2E_COMMANDS` with newline-separated `/exchange` commands and
`E2E_FORM_RESPONSES` with newline-separated JSON form responses. The default
`console_driver` mode routes commands through the temporary Python driver;
direct packet modes remain available for protocol diagnostics.

After testing, stop the server, remove both temporary E2E plugins, rebuild with
`EXCHANGE_BUILD_E2E_DRIVER=OFF`, and perform a clean production-like restart.
