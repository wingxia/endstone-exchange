# Endstone Exchange

Endstone Exchange 是面向 Endstone 的 C++ 物品交易插件。当前正式版本为 **1.9.0**，支持世界交易点、展示框一口价交易、完整 NBT 物品、MySQL 持久化和 UMoney 资金系统。

- 正式版本：[v1.9.0](https://github.com/wingxia/endstone-exchange/releases/tag/v1.9.0)
- 构建状态：[main CI](https://github.com/wingxia/endstone-exchange/actions/workflows/ci.yml?query=branch%3Amain)

## 兼容性

| 组件 | 版本 |
| --- | --- |
| Endstone | 0.11.6 |
| BDS | 1.26.33.1 |
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

## 交易所保护范围

在 `plugins/exchange/config.toml` 中用同一维度的两个三维坐标点设置一个包含边界的长方体。两个点的顺序不限，维度名大小写不敏感；启用后，所有玩家（包括 OP）都不能在范围内破坏、放置或通过普通右键修改方块，爆炸也不会破坏范围内的方块。交易点和挂牌展示框的交易交互仍可正常使用。

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

1. 从 [v1.9.0 Release](https://github.com/wingxia/endstone-exchange/releases/tag/v1.9.0) 下载 `endstone_exchange-v1.9.0-linux-x86_64.so`。
2. 校验发布文件：

   ```bash
   echo "f4f0e6229ca17c55f409d46878bd75ce00c3652510756bbb4dbacc791b7f2690  endstone_exchange-v1.9.0-linux-x86_64.so" | sha256sum --check
   ```

3. 安装 UMoney 260602 正式 wheel。
4. 将发布文件复制到服务端 `plugins/endstone_exchange.so`。
5. 启动一次服务端，生成 `plugins/exchange/config.toml`。
6. 配置 MySQL，并按上方示例启用 UMoney。
7. 将配置文件权限限制为服务账号可读，例如 `chmod 600 plugins/exchange/config.toml`。
8. 重启服务端并执行 `exchange status`，确认 Exchange `1.9.0`、UMoney `260602` 和数据库均可用。

完整配置模板位于 [config/config.example.toml](config/config.example.toml)。

## 构建与测试

```bash
wget https://apt.llvm.org/llvm.sh -O /tmp/llvm.sh
chmod +x /tmp/llvm.sh
sudo /tmp/llvm.sh 20
sudo apt-get install cmake ninja-build pkg-config libc++-20-dev libc++abi-20-dev libmariadb-dev python3-dev

cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-20 \
  -DEXCHANGE_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

MySQL 集成测试必须使用独立测试库：

```bash
EXCHANGE_TEST_DB_CONFIG=/secure/path/config.toml \
ctest --test-dir build --output-on-failure
```

测试会清理测试库中以 `exchange_` 开头的表，测试配置不能指向生产数据库。

## v1.9.0 验证结果

- Clean Release 构建通过。
- CTest 全部通过。
- GitHub Actions `main` 与 `v1.9.0` 标签 CI 全部通过。
- NAS UDP 19141 Endstone/BDS 实机测试通过。
- 展示框挂牌、购买、余额不足、改价、取消和完整 NBT 掉落测试通过。
- `exchanger` 非 OP 拒绝与 OP 创建交易点测试通过。
- 限价订单、直接成交、退款、离线领取和重连测试通过。
- SIGKILL、资金写入中断和连续两次重启恢复测试通过，无重复扣款或重复入账。

正式发布：[Endstone Exchange v1.9.0](https://github.com/wingxia/endstone-exchange/releases/tag/v1.9.0)

| 文件 | SHA-256 |
| --- | --- |
| `endstone-exchange-v1.9.0-linux-x86_64.tar.gz` | `2aea992290c7f758adb8f53a8834015f2d9c2cf171c0a574a85c6f0e44b72966` |
| `endstone_exchange-v1.9.0-linux-x86_64.so` | `f4f0e6229ca17c55f409d46878bd75ce00c3652510756bbb4dbacc791b7f2690` |

## 许可证

[MIT](LICENSE)
