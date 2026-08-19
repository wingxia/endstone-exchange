# Endstone Exchange

Endstone Exchange 是面向 Endstone 的 C++ 物品交易插件。当前正式版本为 **1.10.0**，支持世界交易点、命令方块背包一键出售、展示框一口价交易、交易所区域保护、完整 NBT 物品、MySQL 持久化和 UMoney 资金系统。

- 正式版本：[v1.10.0](https://github.com/wingxia/endstone-exchange/releases/tag/v1.10.0)
- 构建状态：[main CI](https://github.com/wingxia/endstone-exchange/actions/workflows/ci.yml?query=branch%3Amain)

## 兼容性

| 组件 | 版本 |
| --- | --- |
| Endstone | 0.11.7（NAS 回归运行时 0.11.8） |
| BDS | 1.26.40 |
| UMoney | 260602 |
| Linux 编译器 | LLVM/Clang 20 + libc++ |
| 数据库 | MySQL 8 / MariaDB，InnoDB |

## 展示框一口价交易

1. 所有玩家都可以在铁砧中把普通木棍精确命名为 `price`，不需要管理权限。
2. 玩家手持 `price` 右击装有物品的展示框，填写 `1` 到 `5000` 的整数 `u` 价格。
3. 卖家或 OP 可再次右击该展示框修改价格或取消挂牌。
4. 买家左击已挂牌展示框，核对物品、卖家、价格和余额后确认付款。
5. 付款成功后，展示框内物品以完整 NBT 掉落。

挂牌期间展示框物品不能旋转、破坏，也不会被爆炸破坏。余额不足、物品被替换、重复确认或多人同时确认时不会产生重复扣款或重复成交。

## 世界交易点

- OP 使用精确命名为 `exchanger` 的木棍创建或关闭交易点。
- 方块、掉落物、展示框和可映射实体均可作为交易目标。
- 玩家右击交易点即可查看订单簿、余额和可出售数量。
- 支持限价买入、限价卖出、直接购买和直接出售。
- 价格优先级为价格优先、同价时间优先。
- 相同完整 NBT 的物品共享订单，不同物品快照分别交易。
- 简体中文、繁体中文、英文和日文界面按客户端语言自动显示。

## 命令方块背包一键出售

玩家空手或手持普通物品右击命令方块、连锁命令方块或循环命令方块，会看到一键出售确认框。确认后插件按以下规则处理玩家主背包和副手：

1. 只选择当前已有活跃交易点、且完整 NBT 与交易物快照完全相同的物品。
2. 每种物品先按订单簿当前最高买价成交，同价时优先成交更早的订单。
3. 没有买单或未全部成交的余量自动以每件 `1u` 建立限价卖单。
4. 没有被设为交易物的物品、不同 NBT 的物品及护甲栏物品保持不变；副手中的交易物按相同规则出售。
5. 相同完整 NBT 的多个物理交易点共享同一订单簿，只处理一次；数量超过单笔上限时自动拆单。

成交收入自动结算到 UMoney。未成交的 `1u` 卖单可通过 `/exchange orders` 查看或取消，取消后物品由正常的暂存领取流程安全返还。命令方块位于已配置的交易所保护范围内时，此交易入口仍然可用。

## 交易所保护范围

在 `plugins/exchange/config.toml` 中用同一维度的两个三维坐标点设置一个包含边界的长方体。两个点的顺序不限，维度名大小写不敏感；启用后，所有玩家（包括 OP）都不能在范围内破坏、放置或通过普通右键修改方块，爆炸也不会破坏范围内的方块。交易点、挂牌展示框和命令方块一键出售的交易交互仍可正常使用。

```toml
[protection]
enabled = true
dimension = "Overworld"
point1_x = -20
point1_y = 50
point1_z = -20
point2_x = 20
point2_y = 100
point2_z = 20
```

启用时必须填写全部六个坐标；修改后需重启服务端。

## UMoney

Exchange 在 Endstone 主线程直接调用 UMoney 的 `api_get_player_money` 和 `api_change_player_money`，读取和修改 UMoney 的整数余额。订单和交易临时余额在 Exchange 内部按交易分保存，金额单位为 `1 UMoney = 1u = 100 交易分`。玩家不需要手动换算或划转资金。

- 可用余额为 UMoney 余额和交易临时余额的合计；购买时按需直接扣减 UMoney 的整数余额。
- 卖家所得、订单取消、价格改善和失败交易的退款自动结算到 UMoney。
- 不足 1 UMoney 的交易临时余额按交易分保留，累计到整数单位后自动结算到 UMoney。
- UMoney 模式下 `/exchange addbalance` 不可用。

推荐配置：

```toml
[market]
initial_balance = 0

[economy]
provider = "umoney"
umoney_plugin = "umoney"
recovery_interval_ticks = 100
```

## 数据与恢复

- MySQL/InnoDB 保存账户、交易点、订单、成交记录、待领取物品和不可变资金账本。
- UMoney 余额变更保存调用前余额、目标余额和执行状态；重启恢复会区分未执行、已执行和余额冲突。
- 展示框成交使用持久化状态机，服务器中断后可继续完成扣款、清空展示框和生成唯一掉落物。
- 买入交付和卖出暂存使用可恢复状态，玩家离线或背包已满时物品保存在待领取队列。
- 物品匹配与交付包含类型、数据值、自定义名称、说明、附魔、耐久和确定性编码的完整 NBT。

## 命令与权限

| 命令 | 权限 | 作用 |
| --- | --- | --- |
| `/exchange give [在线玩家]` | `exchange.admin` | 发放名为 `exchanger` 的木棍 |
| `/exchange balance` | `exchange.use` | 查询可用余额（UMoney 与交易临时余额合计） |
| `/exchange orders` | `exchange.use` | 查看并取消自己的未完成订单 |
| `/exchange claim` | `exchange.use` | 领取暂存物品 |
| `/exchange addbalance <玩家> <金额>` | `exchange.admin` | 调整内部经济余额（UMoney 模式不可用） |
| `/exchange status` | `exchange.use` | 检查数据库、市场、资金操作和恢复队列 |

`exchange.use` 默认授予所有玩家，`exchange.admin` 默认授予 OP。

## 安装

1. 从 [v1.10.0 Release](https://github.com/wingxia/endstone-exchange/releases/tag/v1.10.0) 下载 `endstone_exchange-v1.10.0-linux-x86_64.so`。
2. 校验发布文件：

   ```bash
   echo "3ad547502ca0a24c4a20a09a18ee4067d67de3f5aab514fe9eebebf6ad73329a  endstone_exchange-v1.10.0-linux-x86_64.so" | sha256sum --check
   ```

3. 使用 Endstone 0.11.7 运行 BDS 1.26.40。
4. 安装 UMoney 260602 正式 wheel。
5. 将发布文件复制到服务端 `plugins/endstone_exchange.so`。
6. 启动一次服务端，生成 `plugins/exchange/config.toml`。
7. 配置 MySQL，并按上方示例启用 UMoney。
8. 将配置文件权限限制为服务账号可读，例如 `chmod 600 plugins/exchange/config.toml`。
9. 重启服务端并执行 `exchange status`，确认 Exchange `1.10.0`、UMoney `260602` 和数据库均可用。

完整配置模板位于 [config/config.example.toml](config/config.example.toml)。

## 构建与测试

```bash
wget https://apt.llvm.org/llvm.sh -O /tmp/llvm.sh
chmod +x /tmp/llvm.sh
sudo /tmp/llvm.sh 20
sudo apt-get install cmake ninja-build pkg-config libc++-20-dev libc++abi-20-dev libmariadb-dev python3-dev

cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-20 \
  -DEXCHANGE_BUILD_TESTS=ON \
  -DEXCHANGE_BUILD_E2E_DRIVER=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

MySQL 集成测试必须使用独立测试库：

```bash
EXCHANGE_TEST_DB_CONFIG=/secure/path/config.toml \
ctest --test-dir build --output-on-failure
```

测试会清理测试库中以 `exchange_` 开头的表，测试配置不能指向生产数据库。

## v1.10.0 验证结果

- LLVM/Clang 20 Clean Release 构建通过，确认 `EXCHANGE_BUILD_E2E_DRIVER=OFF`，CTest 全部通过。
- 2026-08-19 在隔离的 NAS UDP 19141、BDS 1.26.40 与 Endstone 0.11.8 运行时完成真实 Bedrock 假人回归；生产服进程未参与测试。
- 普通、连锁和循环命令方块入口均通过。核心场景中卖家背包有 5 个钻石块与 7 根羽毛，买单以 `5u` 收购 2 个：确认后 2 个成交并获得 `10u`，其余 3 个以 `1u` 挂单，7 根非交易物羽毛完整保留。
- 限价买卖、直接买卖、部分成交、价格优先、取消返还、退款、离线领取、重连、共享订单簿、市场关闭/重开、非 OP 权限拒绝和保护范围内外交互均通过。
- 两次优雅重启后市场、余额和订单状态保持一致；最终卸载全部临时 E2E 驱动并以正式产物重启，恢复 11 个活跃交易点，数据库正常，未完成/待核对直接转账为 0。
- 展示框一口价路径继续由 CTest 与 v1.9.0 的完整 NAS 发布回归覆盖；本版本未修改展示框状态机。

正式发布：[Endstone Exchange v1.10.0](https://github.com/wingxia/endstone-exchange/releases/tag/v1.10.0)

| 文件 | SHA-256 |
| --- | --- |
| `endstone-exchange-v1.10.0-linux-x86_64.tar.gz` | `3f89af9ad525adc5df1bafbb35c481eeb220fa4630922f820e949dc674bbcb70` |
| `endstone_exchange-v1.10.0-linux-x86_64.so` | `3ad547502ca0a24c4a20a09a18ee4067d67de3f5aab514fe9eebebf6ad73329a` |

## 许可证

[MIT](LICENSE)
