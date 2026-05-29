# UE5 Agent Blueprint Tools

- 涉及 Blueprint、Material、Content Browser Asset 或关卡 Actor 放置时，使用 `ue5-blueprint-agent` skill 和 `ue5_agent_blueprint_tools` MCP server。
- 绝不直接编辑 `.uasset`、`.umap` 或其他二进制资源文件。
- 修改 Blueprint 前必须先调用 `read_blueprint`；涉及具体图表时再调用 `analyze_blueprint_graph`。
- 应用 Blueprint patch 前必须先调用 `dry_run_blueprint_patch`；dry-run 成功后才可调用 `apply_blueprint_patch`。
- 修改 Blueprint 后必须检查编译结果；编译失败时如实报告日志，不要声称完成。
- 除非用户明确要求或任务确有需要，否则不要保存资产或关卡；写入工具的 `saveOnSuccess`、`saveAssets`、`saveLevel` 默认保持 `false`。
- 优先使用精简 Blueprint 函数，不把所有逻辑堆在 EventGraph；除非明确要求，避免新增 Tick。
- 不凭空猜 pin 名称；通过 `read_blueprint`、`analyze_blueprint_graph`、`list_callable_functions` 或现有 IR 获取准确名称。
- 优先使用声明式 Patch DSL；只有当插件/MCP 不支持且任务明确需要时，才考虑 C++ 扩展。

# 目录边界

可作为源码/资料入口：

- `Source/`
- `Plugins/AgentBlueprintTools/`
- `Config/`，仅当任务涉及配置
- `Content/`

默认不要阅读、修改或提交：

- `Binaries/`
- `Intermediate/`
- `DerivedDataCache/`
- `Saved/`
- `ArchivedBuilds/`
- `Releases/`
- `Platforms/`
- `Build/`
- `.vs/`
- `.idea/`
- `*.sln`
- `*.user`
- `*.log`
- `*.tmp`
- `*.cache`
- `*.sqlite`
- `*.db`
- `node_modules/`
- `.venv/`
- `dist/`
- `build/`
- `.next/`
- `coverage/`
- `generated/`

# 引擎路径
优先使用环境变量 `UE5_ROOT`；本机验证使用 `F:\EPIC\Engine\Windows`。

# 架构设计
阅读 ArchitectureDesign.md
