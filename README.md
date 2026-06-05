# Endstone Exchange

C++ Endstone 交易所插件。玩家可通过 `/ex` 或 `/exchange` 买卖服务器中的物品；商品按“物品 ID + 附魔签名”区分，完整 NBT 只用于托管和交付，不拆分商品。

## Repository Layout

- `cpp/`：C++ core、UI model、Endstone 插件入口。
- `python/umoney_bridge/`：极薄 UMoney bridge，给 C++ 插件提供本机 balance/debit/credit HTTP 接口。
- `resources/items.yml`：商品分类、名称、图标和关键词配置。
- `migrations/001_init.sql`：MySQL/InnoDB 表结构。
- `tests/`：C++ core 测试和 Python bridge 测试。

## Runtime Model

- MySQL 保存商品目录、订单簿、托管物品、冻结金额、成交流水、邮箱、结算任务和管理员审计。
- 买单先通过 UMoney bridge 扣款，再写入 `exchange_reserved_funds`。
- 卖单立即扣物品并写入 `exchange_item_lots`。成交后物品进入买家邮箱，卖家入账进入 `exchange_settlement_jobs`。
- 管理员补货使用系统卖单池，适合基建材料固定价格补货。

## Build Core

```bash
cmake -S . -B build -DEXCHANGE_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests/python
```

Endstone 插件 target 预留为：

```bash
cmake -S . -B build-plugin -DEXCHANGE_BUILD_ENDSTONE_PLUGIN=ON -DEXCHANGE_BUILD_TESTS=OFF
cmake --build build-plugin
```

## UMoney Bridge

Bridge 配置在服务器工作目录：

```text
plugins/exchange_umoney_bridge/config.yml
```

首次启动会生成 token。C++ 插件配置应使用同一个 token 访问 `127.0.0.1:8765`。

HTTP 接口：

- `GET /balance?player=<name>`
- `POST /debit` with `player`, `amount`, `idempotency_key`
- `POST /credit` with `player`, `amount`, `idempotency_key`

所有请求都需要 `Authorization: Bearer <token>`。

## Current Implementation Notes

Core service and tests are implemented. The MySQL schema and service interfaces are complete; the production MySQL repository can be attached behind the `Repository` interface without changing UI or matching behavior.

