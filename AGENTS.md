## UE5 Agent Blueprint Tools

- 涉及 Blueprint、Material、Content Browser Asset 或关卡 Actor 放置时，使用 `ue5-blueprint-agent` skill 和 `ue_editor` MCP server。
- 绝不直接编辑 `.uasset`、`.umap` 或其他二进制资源文件。
- 修改 Blueprint 前必须先调用 `read_blueprint`；涉及具体图表时再调用 `analyze_blueprint_graph`。
- 应用 Blueprint patch 前必须先调用 `dry_run_blueprint_patch`；dry-run 成功后才可调用 `apply_blueprint_patch`。
- 修改 Blueprint 后必须检查编译结果；编译失败时如实报告日志，不要声称完成。
- 除非用户明确要求或任务确有需要，否则不要保存资产或关卡；写入工具的 `saveOnSuccess`、`saveAssets`、`saveLevel` 默认保持 `false`。
- 优先使用精简 Blueprint 函数，不把所有逻辑堆在 EventGraph；除非明确要求，避免新增 Tick。
- 不凭空猜 pin 名称；通过 `read_blueprint`、`analyze_blueprint_graph`、`list_callable_functions` 或现有 IR 获取准确名称。
- 优先使用声明式 Patch DSL；只有当插件/MCP 不支持且任务明确需要时，才考虑 C++ 扩展。


## Project Memory 强制门禁

- 下列任何任务开始前，必须先调用 `project_memory` MCP tool `memory_status`，再调用 `get_context_pack`：功能实现、bug 修复、重构、UE C++、蓝图修改、材质修改、资产创建、UI 功能、输入系统、AI/Combat/Inventory/Interaction 等 gameplay 系统、系统设计、项目架构理解。
- 如果 `memory_status` 返回 `missing`、`stale_config`、`stale_sources`、`stale_graph` 或 `needs_full_rebuild`，先调用 `update_project_memory`；如果工具提示必须完整重建，再调用 `build_project_memory`。
- 修改前必须基于 `get_context_pack` 明确说明：owning system、related files、related blueprints、related assets、existing patterns、可复用变量/类/组件/蓝图/DataAsset、validation plan。
- 禁止未查询 Project Memory 就开始功能实现、bug 修复、重构、系统设计或架构判断。
- 禁止随便新建 Manager / Subsystem；除非 context pack 明确说明没有已有 owner，且先说明为什么现有 owner 不适用。
- 优先复用现有变量、类、组件、蓝图、DataAsset、patterns 和既有系统边界。
- 禁止直接编辑 `.uasset`、`.umap` 或其他二进制资源文件。
- 禁止猜测蓝图、材质或资产内容；必须通过 Project Memory、ProjectGraph 返回的 IR、UE MCP 读取工具或 patch 工具确认。
- 蓝图、材质和资产修改必须使用可用的 UE MCP tools、`ue_editor` MCP server 或项目认可的 patch DSL；先 read/analyze，再 dry-run，再 apply，最后验证。
- 修改后必须调用 `impact_analysis` 分析影响范围，并对资料或配置变更调用 `update_project_memory` 更新缓存。


## ProjectGraph 底层工具边界

- `ue_project_graph` 是低层图谱工具，优先通过 `project_memory` 获取汇总上下文。
- 只有在 Project Memory 明确缺少图谱证据、需要底层节点/边细节、或需要重建/更新图谱时，才直接调用 `ue_project_graph` 的 `find_feature_context`、`get_context_pack`、`query_graph`、`impact_analysis`、`build_project_graph` 或 `update_project_graph`。
- ProjectGraph 输出只摘要关键 seed、owner、文件和验证计划；不要在回复或上下文中展开大段 JSON、完整 edges/nodes 或无关候选。
- 如果图谱缺失，先调用 `build_project_graph` 或 `update_project_graph`；仍不可用时说明阻塞原因，并继续基于 Project Memory 文档证据处理非图谱依赖部分。

## 低噪声执行规则

- 按任务规模选择最小闭环：小改动只定位 owner、读关键文件、做最小修改、跑必要验证、简短汇报。
- 搜索必须先限定目录和关键词，优先 `Source/`、`Plugins/`、`Script/`、`Config/` 中的相关子目录；避免扫 `Intermediate/`、`Saved/`、插件生成物或全仓库宽泛关键词。
- 工具输出必须限流：使用 `rg -n`、`Select-String`、`Get-Content -TotalCount/-Tail`、`max_output_tokens`，只保留能支撑决策的行号和摘要。
- 遇到长日志或大 JSON，只提取 `Error`、`Warning`、目标符号、最终结果和相关行号，不粘贴整段原始输出。
- 验证按风险升级：先跑最快可用验证；只有 C++/UFUNCTION/API 签名、`.as`、配置或跨模块改动才升级到 Editor Target、编辑器加载或打包脚本。
- 中间进展只报告关键发现、正在修改什么、验证结果；避免把检索过程逐步展开成冗长流水账。
- 最终回复控制在高信号信息：实现思路、修改文件、验证命令与结果、未验证风险；不要复述无关日志。

# 目录边界

可作为源码/资料入口：

- `Source/`
- `Plugins/AgentBlueprintTools/`
- `Plugins/AgentProjectGraph/`
- `AgentTools/`
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
- 蓝图、材质和资产修改必须使用 `ue_editor` MCP tools 或项目认可的 patch DSL；先 read/analyze，再 dry-run，再 apply，最后验证。

# 架构设计
阅读 `AgentTools/AgentBlueprintTools/docs/ARCHITECTURE_DESIGN.md` 和 `AgentTools/README.md`
