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

## ProjectGraph 强制门禁

- 下列任何任务开始前，必须先调用 ProjectGraph MCP tool `find_feature_context`，再调用 `get_context_pack`：新功能、bug 修复、重构、蓝图修改、材质修改、资产创建、UI 功能、输入系统、AI/Combat/Inventory/Interaction 等 gameplay 系统。
- 修改前必须基于 `get_context_pack` 明确说明：owning system、related files、related blueprints、related assets、existing pattern to follow、validation plan。
- 禁止未查图谱就改代码；如果图谱缺失，先调用 `build_project_graph` 或 `update_project_graph`，仍不可用时停止并说明阻塞原因。
- 禁止新建 Manager / Subsystem，除非 context pack 明确说明没有已有 owner，且先说明为什么现有 owner 不适用。
- 禁止直接编辑 `.uasset`、`.umap` 或其他二进制资源文件。
- 禁止猜测蓝图、材质或资产内容；必须通过 ProjectGraph 返回的 IR、UE MCP 读取工具或 patch 工具确认。
- 蓝图、材质和资产修改必须使用 `ue_editor` MCP tools、`ue5_agent_blueprint_tools` MCP server 或项目认可的 patch DSL；先 read/analyze，再 dry-run，再 apply，最后验证。

# 架构设计
阅读 ArchitectureDesign.md
