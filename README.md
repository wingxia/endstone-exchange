# Endstone Exchange

一个面向 Endstone 的 C++ 物品交易插件。OP 用名为 `exchanger` 的木棍把世界中的方块或实体设为交易点，玩家直接右击目标即可查看价格并交易；所有玩家还可用名为 `price` 的木棍在展示框中进行一口价交易。当前插件版本为 **1.9.0**，锁定 **Endstone v0.11.6 / BDS 1.26.33.1**。

## 功能

- 只有 OP 能用名为 `exchanger` 的木棍右击目标：开启交易；再次右击：关闭这个交易点，但保留未完成订单以及暂存的余额和物品。即使另行授予 `exchange.admin`，非 OP 也不能设置交易点。
- 所有玩家都可把普通木棍在铁砧中命名为 `price`，右击装有物品的展示框并填写整数 `u` 价格。挂牌后展示框禁止旋转、破坏和爆炸掉落；卖家或 OP 可再次用 `price` 右击改价或取消。
- 买家左击已挂牌展示框会看到物品、卖家、价格与自身可用余额，确认后原子扣买家余额并增加卖家余额，随后保留完整 NBT 的展示物从框中掉落。改价、换物、重复确认、余额不足和两个买家同时确认都不会产生重复扣款或重复成交。
- 展示框一口价成交使用带隐藏标记的持久化状态机；扣款后即使服务器在清空展示框或生成掉落物时中断，重启也会从 `PAID` / `DROPPED` 状态续做。成交掉落物不会自然消失，拾取后才释放该展示框位置。
- 普通右击交易目标：在一块普通字号的紧凑正文中显示“有人收购”、“有人出售”、余额和可出售数量。两类订单各最多显示前两个价格，更多价格用省略号提示；页面不再堆叠大标题、分隔线、空行和不可用说明。只有存在出售订单时才显示“直接购买”，只有存在收购订单时才显示“直接出售”，并把可直接成交的操作排在前面。
- 选择交易方式后，用输入框填写整数数量；按自己的价格交易时再填写 `1～5000` 的整数价格，直接购买或直接出售时只填写数量。确认页只保留“确认交易”和“修改填写”两个按钮，并明确显示数量、整数 `u` 价格和最大金额，不再要求玩家向下滚动寻找确认按钮。
- 四种交易方式：按自己的价格购买、按自己的价格出售、直接购买、直接出售。关闭表单后的下一 tick 才切换页面或执行订单，完成后重发主背包与副手，避免客户端残留表单或“幽灵物品”。
- 自动读取客户端语言，完整支持简体中文、英文、繁体中文和日文；未知语言回退到简体中文。聊天、命令反馈、交易表单、订单、物品/NBT 校验说明、exchanger 描述和恢复提示都会自动适配。普通物品名复用 Minecraft 自带翻译，自定义物品名保持原样。
- 价格更合适的订单优先，同价时先提交的优先；购买时暂存余额，出售时暂存实际物品。
- 方块按对应物品交易；掉落物保留其物品；其他实体尽可能映射为对应物品或刷怪蛋。
- 展示框有物品时交易展示物并保留 NBT；空展示框交易展示框本身。
- 目标上方只显示物品名、“有人收购/有人出售”的价格及数量，价格使用整数 `u`（例如 `10u x 90`）；展示框会按朝向定位其附着方块，并把文字放在附着方块上方。区块加载后会自动去重。若玩家重进时目标区块本来就已加载，插件会延迟重建一次附近载体，保证客户端先收到实体再收到显示数据；新加载的区块不会多做这次重建。
- 悬浮文字只在生成或目标实体真正移动时定位；方块上的文字不会再被每秒传送，因此不会上下跳动。
- 悬浮文字通过按玩家发送的实体元数据显示各自语言，同一交易点旁的不同语言玩家可同时看到本地化的“有人收购/有人出售”文字。
- MySQL/InnoDB 持久化账户、交易点、订单、交易记录和待领取物品；支持离线领取与重启恢复。
- 金额全程使用整数分，物品身份包含类型、数据值和确定性编码的完整 NBT。
- 每个世界位置都有独立开关；多个位置交易完全相同的物品时共享订单，关闭一个位置不会影响其他位置，也不会取消订单。
- 交易物品快照不可变；同一目标以相同物品重新开启会恢复原交易点，物品发生变化时才建立新的交易记录。
- 出售时直接扫描主背包与副手，并按类型、数据值和完整 NBT 校验，不依赖 Endstone 的运行时 `ItemMeta` 相似性状态。
- 交易界面只显示真正需要匹配的特殊条件，例如非零数据值、自定义名称、说明、附魔名称和等级、非零耐久损耗、铁砧修复代价以及其他特殊属性；数据值 `0`、无名称、无说明、无附魔、零损耗等默认状态不再显示。出售失败时复用同一份清单，完整 NBT 精确匹配规则保持不变。
- 购买物品的领取和出售物品的暂存均使用可恢复的两阶段状态机，服务器中断后通过隐藏 NBT 标记继续或回滚。
- 余额变动写入不可变账本，可校验账本总和与账户余额。
- 可选直接接入 [UMoney](https://github.com/U-Blocks/UMoney)：购买时自动从 UMoney 扣款，卖家所得、价格改善退款和取消订单退款自动结算回 UMoney，不需要充值、提现命令或 HTTP bridge。MySQL 保存调用前余额和转账状态，并在 UMoney 文件刷盘后完成账本，使进程强杀与重启恢复不会重复扣款或重复入账。
- 悬浮交易提示使用异步批量快照，查询量不再按 `3 × 交易点数` 增长；插件只在目标区块已加载且经过 20 tick 实体恢复期后才补建载体，并在区块加载、跨区块移动和玩家重进后分阶段补发显示数据。

## Endstone API 可行性

右击方块、右击实体、读取命名物品、操作背包、显示表单、生成盔甲架和调度任务均已有公开 API。Endstone v0.11.6 唯一缺口是 `Block`/`BlockState` 尚不能直接读取展示框中的物品。

展示框采用兼容层处理：插件通过公开命令保存一个带随机名称的单方块结构，等待 80 tick 让 BDS 写入世界 LevelDB，读取 `structuretemplate_<namespace:name>` 的 BlockActor NBT，然后立即删除该结构。若 LevelDB 的结构记录尚未可见，会以相同名称重新保存并最多重试三次；读取期间同一展示框会暂时拒绝其他操作。`exchanger` 设置交易点时空 `Item` 标签回退为展示框物品，`price` 挂牌则明确要求框中有物品。成交时使用原始 `BlockData` 无物理更新地重建展示框来清空 BlockActor，再验证空框后生成带挂牌编号的唯一掉落物，避免 BDS 把展示框误当作命令容器。详细接口清单见 [docs/api-feasibility.md](docs/api-feasibility.md)。

## 构建

Linux 上必须使用 Endstone v0.11.6 同款的 LLVM/Clang 20 与 libc++，以匹配 ABI，并避开旧版 libc++ 的 `std::format` 不兼容。以 Debian/Ubuntu 为例：

```bash
wget https://apt.llvm.org/llvm.sh -O /tmp/llvm.sh
chmod +x /tmp/llvm.sh
sudo /tmp/llvm.sh 20
sudo apt-get install cmake ninja-build pkg-config libc++-20-dev libc++abi-20-dev libmariadb-dev python3-dev
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++-20 -DEXCHANGE_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CMake 会固定获取 Endstone `v0.11.6` 和与其一致的 pybind11 `v3.0.1`。产物为 `build/endstone_exchange.so`，UMoney 模式不产生也不需要额外桥接包。

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
| `initial_balance` | 新账户初始余额 |
| `max_order_quantity` | 单笔最大数量，默认 640 |
| `price_min` / `price_max` / `price_step` | 整数 `u` 价格输入范围和有效步长，默认 1～5000、步长 1 |
| `order_book_depth` | 每边读取多少个价格；交易首页最多显示前两个，更多用省略号提示 |
| `hologram_refresh_ticks` | 浮字刷新周期 |
| `frame_capture_delay_ticks` | 展示框结构写入 LevelDB 的等待 tick 数 |

配置文件应只允许服务账号读取，例如 `chmod 600 plugins/exchange/config.toml`。

### UMoney 模式

1. 安装 UMoney 的正式 wheel 和 `endstone_exchange.so`，不安装任何 Exchange bridge。
2. 将 `economy.provider` 改为 `"umoney"`，保持 `economy.umoney_plugin = "umoney"`，并把 `market.initial_balance` 设为 `0`。
3. 重启后执行 `exchange status`，经济来源应显示 `umoney`，未完成/待核对直接转账应为 `0`。

固定换算为 `1 UMoney = 1u = 100` 交易分。Exchange 在 Endstone 主线程直接调用 UMoney 的公开 `api_get_player_money` 和 `api_change_player_money`；购买前自动补足临时交易余额，成交、退款或取消后自动把可用余额结算回 UMoney。玩家不需要也不能使用 `/exchange deposit` 或 `/exchange withdraw`。

每次直接调用前，Exchange 会在同一笔 MySQL 准备事务中固化 UMoney 当前余额。若服务器在调用附近中断，恢复时只在“仍是调用前余额”时执行，在“已经是预期调用后余额”时仅补完本地事务；其他余额一律冻结等待核对。这样无需网络 bridge 也不会在未知状态下猜测重试。

UMoney 模式会核对“账户余额 + 未完成买单暂存 + 待处理提现”与历史净充值。若从旧版内部经济直接切换且仍有未背书余额或买单，插件会拒绝启动，避免把旧余额铸造成 UMoney；应先在备份上清算、重置或完成有实际 UMoney 储备的迁移。该模式下 `/exchange addbalance` 也会被禁用。

模块边界、数据库不变量、升级路线和故障注入测试清单见 [docs/architecture.md](docs/architecture.md)。

## 使用

| 命令 | 权限 | 作用 |
| --- | --- | --- |
| `/exchange give [在线玩家]` | `exchange.admin` | 发放名为 `exchanger` 的木棍 |
| `/exchange balance` | `exchange.use` | 查询可用余额；UMoney 模式包含 UMoney 与尚未扫回的临时余额 |
| `/exchange orders` | `exchange.use` | 打开自己的未完成订单并可取消 |
| `/exchange claim` | `exchange.use` | 领取因背包已满或离线而暂存的物品 |
| `/exchange addbalance <在线玩家> <金额>` | `exchange.admin` | 调整余额，可传负数但不能扣成负余额 |
| `/exchange status` | `exchange.use` | 检查数据库、活跃市场、资金网关和恢复队列 |

`exchange.use` 默认所有玩家拥有；`exchange.admin` 默认仅 OP 拥有。

`price` 不需要任何插件权限：玩家在铁砧中把普通木棍精确命名为 `price` 即可。启用 UMoney 时，购买会直接扣 UMoney，卖家所得和退款会自动回到 UMoney。`exchanger` 的世界交易点设置则硬性要求玩家为 OP，不受权限节点或旧配置中的 `admin_only` 值影响。

管理员用 exchanger 再次右击只会关闭该位置的交易点，未完成订单和暂存内容不会改变；其他展示相同物品的位置仍能共享这些订单，以完全相同的物品重新开启后也会继续交易。若展示框物品发生变化，则建立独立的新记录，旧订单仍可通过 `/exchange orders` 单独取消。目标被破坏或目标实体被移除时，相关未完成购买会退款，未完成出售会进入待领取物品。玩家加入、打开交易界面或执行 `/exchange claim` 时会继续领取；背包不足时不会丢失数据库中的未领取数量。

## 许可证

[MIT](LICENSE)
