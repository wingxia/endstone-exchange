# Endstone Exchange

一个面向 Endstone 的 C++ 物品交易插件。管理员用名为 `exchanger` 的木棍把世界中的方块或实体设为交易点，玩家直接右击目标即可查看价格并交易。当前插件版本为 **1.6.0**，锁定 **Endstone v0.11.6 / BDS 1.26.33**。

## 功能

- 名为 `exchanger` 的木棍右击目标：开启交易；再次右击：关闭这个交易点，但保留未完成订单以及暂存的余额和物品。
- 普通右击交易目标：在一块普通字号的紧凑正文中显示收购、出售、余额和可出售数量。收购与出售各最多显示前两个价格，更多价格用省略号提示；页面不再堆叠大标题、分隔线、空行和不可用说明。只有存在出售订单时才显示“直接购买”，只有存在收购订单时才显示“直接出售”，并把可直接成交的操作排在前面。
- 选择交易方式后，用输入框填写整数数量；按自己的价格交易时再填写 `1～5000` 的整数价格，直接购买或直接出售时只填写数量。确认页只保留“确认交易”和“修改填写”两个按钮，并明确显示数量、整数 `u` 价格和最大金额，不再要求玩家向下滚动寻找确认按钮。
- 四种交易方式：按自己的价格购买、按自己的价格出售、直接购买、直接出售。关闭表单后的下一 tick 才切换页面或执行订单，完成后重发主背包与副手，避免客户端残留表单或“幽灵物品”。
- 自动读取客户端语言，完整支持简体中文、英文、繁体中文和日文；未知语言回退到简体中文。聊天、命令反馈、交易表单、订单、物品/NBT 校验说明、exchanger 描述和恢复提示都会自动适配。普通物品名复用 Minecraft 自带翻译，自定义物品名保持原样。
- 价格更合适的订单优先，同价时先提交的优先；购买时暂存余额，出售时暂存实际物品。
- 方块按对应物品交易；掉落物保留其物品；其他实体尽可能映射为对应物品或刷怪蛋。
- 展示框有物品时交易展示物并保留 NBT；空展示框交易展示框本身。
- 目标上方只显示物品名、收购/出售价格及数量，价格使用整数 `u`（例如 `10u x 90`）；展示框会按朝向定位其附着方块，并把文字放在附着方块上方。区块加载后会自动去重。若玩家重进时目标区块本来就已加载，插件会延迟重建一次附近载体，保证客户端先收到实体再收到显示数据；新加载的区块不会多做这次重建。
- 悬浮文字只在生成或目标实体真正移动时定位；方块上的文字不会再被每秒传送，因此不会上下跳动。
- 悬浮文字通过按玩家发送的实体元数据显示各自语言，同一交易点旁的不同语言玩家可同时看到本地化的“收购/出售”文字。
- MySQL/InnoDB 持久化账户、交易点、订单、交易记录和待领取物品；支持离线领取与重启恢复。
- 金额全程使用整数分，物品身份包含类型、数据值和确定性编码的完整 NBT。
- 每个世界位置都有独立开关；多个位置交易完全相同的物品时共享订单，关闭一个位置不会影响其他位置，也不会取消订单。
- 交易物品快照不可变；同一目标以相同物品重新开启会恢复原交易点，物品发生变化时才建立新的交易记录。
- 出售时直接扫描主背包与副手，并按类型、数据值和完整 NBT 校验，不依赖 Endstone 的运行时 `ItemMeta` 相似性状态。
- 交易界面只显示真正需要匹配的特殊条件，例如非零数据值、自定义名称、说明、附魔名称和等级、非零耐久损耗、铁砧修复代价以及其他特殊属性；数据值 `0`、无名称、无说明、无附魔、零损耗等默认状态不再显示。出售失败时复用同一份清单，完整 NBT 精确匹配规则保持不变。
- 购买物品的领取和出售物品的暂存均使用可恢复的两阶段状态机，服务器中断后通过隐藏 NBT 标记继续或回滚。
- 余额变动写入不可变账本，可校验账本总和与账户余额。
- 悬浮交易提示使用异步批量快照，查询量不再按 `3 × 交易点数` 增长；插件只在目标区块已加载且经过 20 tick 实体恢复期后才补建载体，并在区块加载、跨区块移动和玩家重进后分阶段补发显示数据。

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

若要运行 MySQL 集成测试，先创建专用测试库，并让测试读取一个权限为 `600` 的本地插件配置文件：

```bash
EXCHANGE_TEST_DB_CONFIG=/secure/path/config.toml \
ctest --test-dir build --output-on-failure
```

测试会清空 `endstone_exchange_test` 中以 `exchange_` 开头的测试表，不要把测试变量指向生产库。

## 安装与配置

1. 将 `endstone_exchange.so` 放入 Endstone 服务端的 `plugins/`。
2. 首次启动会生成 `plugins/exchange/config.toml`，也可复制 [config/config.example.toml](config/config.example.toml)。
3. 填写 MySQL 连接信息。生产环境建议为插件建立仅能访问插件数据库的独立用户，不要把真实密码提交到 Git。
4. 重启服务端。插件按照 `exchange_schema_versions` 自动执行可重试的版本化迁移，无需手工导入 SQL；`migrations/` 下文件仅供审计。
5. 控制台执行 `exchange status`，应显示数据库正常。

关键市场配置：

| 配置 | 说明 |
| --- | --- |
| `admin_only` | 只有 `exchange.admin` 才能用 exchanger 标记目标 |
| `initial_balance` | 新账户初始余额 |
| `max_order_quantity` | 单笔最大数量，默认 640 |
| `price_min` / `price_max` / `price_step` | 整数 `u` 价格输入范围和有效步长，默认 1～5000、步长 1 |
| `order_book_depth` | 每边读取多少个价格；交易首页最多显示前两个，更多用省略号提示 |
| `hologram_refresh_ticks` | 浮字刷新周期 |
| `frame_capture_delay_ticks` | 展示框结构写入 LevelDB 的等待 tick 数 |

配置文件应只允许服务账号读取，例如 `chmod 600 plugins/exchange/config.toml`。

模块边界、数据库不变量、升级路线和故障注入测试清单见 [docs/architecture.md](docs/architecture.md)。

## 使用

| 命令 | 权限 | 作用 |
| --- | --- | --- |
| `/exchange give [在线玩家]` | `exchange.admin` | 发放名为 `exchanger` 的木棍 |
| `/exchange balance` | `exchange.use` | 查询交易余额 |
| `/exchange orders` | `exchange.use` | 打开自己的未完成订单并可取消 |
| `/exchange claim` | `exchange.use` | 领取因背包已满或离线而暂存的物品 |
| `/exchange addbalance <在线玩家> <金额>` | `exchange.admin` | 调整余额，可传负数但不能扣成负余额 |
| `/exchange status` | `exchange.use` | 检查数据库与活跃市场数 |

`exchange.use` 默认所有玩家拥有；`exchange.admin` 默认仅 OP 拥有。

管理员用 exchanger 再次右击只会关闭该位置的交易点，未完成订单和暂存内容不会改变；其他展示相同物品的位置仍能共享这些订单，以完全相同的物品重新开启后也会继续交易。若展示框物品发生变化，则建立独立的新记录，旧订单仍可通过 `/exchange orders` 单独取消。目标被破坏或目标实体被移除时，相关未完成购买会退款，未完成出售会进入待领取物品。玩家加入、打开交易界面或执行 `/exchange claim` 时会继续领取；背包不足时不会丢失数据库中的未领取数量。

## 许可证

[MIT](LICENSE)
