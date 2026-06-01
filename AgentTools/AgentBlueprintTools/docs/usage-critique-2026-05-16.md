# AIAgentTool 使用批判与优化记录

日期：2026-05-16

## 本轮真实使用面

- 创建并读取 `UserDefinedStruct`，覆盖 float/vector 字段与默认值。
- 创建并读取 `BlueprintInterface`，验证接口类资产能通过统一资产入口创建。
- 创建 Actor Blueprint，写入变量、函数，并生成 Timer 驱动的循环移动逻辑。
- 创建 Material 与 MaterialInstanceConstant，写入表达式连接和实例参数。
- 将生成的 Actor Blueprint 以 transient 方式放入当前编辑器关卡，验证关卡放置路径。

## 使用痛点

- `read_blueprint` 默认导出完整 graph/node/pin IR，简单判断资产结构时 token 成本过高。
- 常见循环移动测试如果让 agent 手写节点和 pin，patch 长、易错、需要多轮 dry-run。
- `dry_run_blueprint_patch` 之前只校验 target/operations 形状，缺少操作级必填字段检查。
- `saveOnSuccess` 对 Blueprint patch 只标记 dirty，返回值容易让调用方误以为已经落盘。
- 创建接口资产没有统一入口，导致结构体、材质、蓝图接口这些基础资产不能用同一套测试闭环覆盖。

## 已做优化

- `read_blueprint` 增加 compact summary 模式，MCP 默认 summary；需要节点细节时再请求 full。
- 新增 `ensure_looping_move` 高层 Blueprint op：内部生成 `MoveByDelta`，并用 `BeginPlay -> Set Timer by Function Name` 循环调用。
- `dry_run_blueprint_patch` 增加操作级字段校验和未知 op 报错，减少 apply 后才失败的往返。
- `apply_blueprint_patch` 在操作失败或编译失败时取消事务，并将 `saveOnSuccess` 改为真实 `SaveLoadedAsset`。
- `create_asset` 支持 `BlueprintInterface`，`read_asset` 返回 Blueprint 类型、父类和生成类信息。
- 自动化测试扩展到结构体、接口、循环移动 Actor、材质写入、材质实例参数和关卡放置。

## 后续建议

- 为 Blueprint callable function 建 per-project schema cache，避免每次大量反射扫描。
- 将 Material patch 也拆成 dry-run/apply 两步，和 Blueprint 工作流保持一致。
- 给 patch DSL 增加 `template` 层，例如 `moving_actor_test`、`ui_circle_material`，进一步压缩 agent 请求。
- 为失败编译日志做结构化摘要，优先返回 error/warning 的节点、graph、message，避免整段日志占用上下文。
