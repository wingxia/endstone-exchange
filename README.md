# Endstone Exchange

一个面向 Endstone 的 C++ 实体交易所插件。管理员用名为 `exchanger` 的木棍把世界中的方块或实体切换为交易目标，玩家直接右击目标即可查看盘口并交易。当前插件版本为 **1.1.1**，锁定 **Endstone v0.11.6 / BDS 1.26.33**。

## 功能

- 名为 `exchanger` 的木棍右击目标：建立市场；再次右击：关闭市场并原子撤销全部挂单。
- 普通右击交易目标：打开包含买卖盘、余额、交易方式、数量滑条和限价滑条的原生 `ModalForm`。限价滑条只传递小范围整数档位，再由服务器映射为整数分，避免高价时的浮点精度损失。
- 四种交易方式：限价买入、限价卖出、直接买入（吃卖盘）、直接卖出（吃买盘）。
- 价格优先、同价时间优先；买单托管余额，卖单托管实际物品。
- 方块按对应物品交易；掉落物保留其物品；其他实体尽可能映射为对应物品或刷怪蛋。
- 展示框有物品时交易展示物并保留 NBT；空展示框交易展示框本身。
- 目标上方用不可见盔甲架显示物品名、买一、卖一及数量；目标区块重新加载后会自动恢复。
- MySQL/InnoDB 持久化账户、市场、挂单、成交和待领取物品；支持离线交割与重启恢复。
- 金额全程使用整数分，物品身份包含类型、数据值和确定性编码的完整 NBT。
- 市场物品快照不可变；同一目标重新开放会建立新代次，不会改写旧挂单或交割物品。
- 买入交割和卖出托管均使用可恢复的两阶段状态机，服务器中断后通过隐藏 NBT 标记继续或回滚。
- 余额变动写入不可变账本，可校验账本总和与账户余额。
- 盘口浮字使用异步批量快照，查询量不再按 `3 × 市场数` 增长。

## Endstone API 可行性

右击方块、右击实体、读取命名物品、操作背包、显示表单、生成盔甲架和调度任务均已有公开 API。Endstone v0.11.6 唯一缺口是 `Block`/`BlockState` 尚不能直接读取展示框中的物品。

展示框采用兼容层处理：插件通过公开命令保存一个带随机名称的单方块结构，等待 80 tick 让 BDS 写入世界 LevelDB，读取 `structuretemplate_<namespace:name>` 的 BlockActor NBT，然后立即删除该结构。读取期间再次用 exchanger 右击会取消设置；空 `Item` 标签自动回退为展示框物品。详细接口清单见 [docs/api-feasibility.md](docs/api-feasibility.md)。

## 构建

Linux 上必须使用 Endstone v0.11.6 同款的 LLVM/Clang 20 与 libc++，以匹配 ABI，并避开旧版 libc++ 的 `std::format` 不兼容。以 Debian/Ubuntu 为例：

```bash
wget https://apt.llvm.org/llvm.sh -O /tmp/llvm.sh
chmod +x /tmp/llvm.sh
sudo /tmp/llvm.sh 20
sudo apt-get install cmake ninja-build pkg-config libc++-20-dev libc++abi-20-dev libmariadb-dev
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++-20 -DEXCHANGE_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CMake 会固定获取 Endstone `v0.11.6`。产物为 `build/endstone_exchange.so`。

若要运行 MySQL 集成测试，先创建专用测试库，再设置：

```bash
EXCHANGE_TEST_DB_HOST=127.0.0.1 \
EXCHANGE_TEST_DB_PORT=3306 \
EXCHANGE_TEST_DB_USER=root \
EXCHANGE_TEST_DB_PASSWORD='test-password' \
ctest --test-dir build --output-on-failure
```

测试会清空 `endstone_exchange_test` 中以 `exchange_` 开头的测试表，不要把测试变量指向生产库。

## 安装与配置

1. 将 `endstone_exchange.so` 放入 Endstone 服务端的 `plugins/`。
2. 首次启动会生成 `plugins/exchange/config.toml`，也可复制 [config/config.example.toml](config/config.example.toml)。
3. 填写 MySQL 连接信息。生产环境建议为插件建立仅能访问插件数据库的独立用户，不要把真实密码提交到 Git。
4. 重启服务端。插件按照 `exchange_schema_versions` 自动执行可重试的版本化迁移，无需手工导入 SQL；`migrations/` 下文件仅供审计。
5. 控制台执行 `exchange status`，应显示 `database=OK`。

关键市场配置：

| 配置 | 说明 |
| --- | --- |
| `admin_only` | 只有 `exchange.admin` 才能用 exchanger 标记目标 |
| `initial_balance` | 新账户初始余额 |
| `max_order_quantity` | 单笔最大数量 |
| `price_min` / `price_max` / `price_step` | 限价滑条和服务端校验范围 |
| `order_book_depth` | 交易界面展示的盘口档数 |
| `hologram_refresh_ticks` | 浮字刷新周期 |
| `frame_capture_delay_ticks` | 展示框结构写入 LevelDB 的等待 tick 数 |

配置文件应只允许服务账号读取，例如 `chmod 600 plugins/exchange/config.toml`。

模块边界、数据库不变量、升级路线和故障注入测试清单见 [docs/architecture.md](docs/architecture.md)。

## 使用

| 命令 | 权限 | 作用 |
| --- | --- | --- |
| `/exchange give [在线玩家]` | `exchange.admin` | 发放名为 `exchanger` 的木棍 |
| `/exchange balance` | `exchange.use` | 查询交易所余额 |
| `/exchange orders` | `exchange.use` | 打开自己的挂单并可撤单 |
| `/exchange claim` | `exchange.use` | 领取因背包已满或离线而暂存的物品 |
| `/exchange addbalance <在线玩家> <金额>` | `exchange.admin` | 调整余额，可传负数但不能扣成负余额 |
| `/exchange status` | `exchange.use` | 检查数据库与活跃市场数 |

`exchange.use` 默认所有玩家拥有；`exchange.admin` 默认仅 OP 拥有。

关闭市场、目标被破坏或目标实体被移除时，未成交买单会退款，未成交卖单会进入待领取物品。玩家加入、打开市场或执行 `/exchange claim` 时会继续交割；背包不足时不会丢失数据库中的未领取数量。

## 许可证

[MIT](LICENSE)
