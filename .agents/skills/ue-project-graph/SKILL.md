---
name: ue-project-graph
description: 当任务涉及 UE C++、蓝图、资产、材质、配置、新功能、bug 修复、重构、UI 功能、输入系统、资产创建，或 AI/Combat/Inventory/Interaction 等 gameplay 系统时触发；必须先用 ProjectGraph MCP 定位上下文再实施
allow_implicit_invocation: true
---

# UE Project Graph Skill

使用本 skill 时，先通过 ProjectGraph MCP tools 定位最小上下文，再按项目原架构实施。ProjectGraph 是只读定位工具；它不能替代 UE MCP / patch DSL 对蓝图、材质和资产进行修改。

## 适用范围

- UE C++、`.Build.cs`、`.Target.cs`
- 蓝图、材质、Content Browser asset、关卡 Actor
- `Config/`、`Script/`
- 新功能、bug 修复、重构、影响面分析
- UI 功能、输入系统
- AI、Combat、Inventory、Interaction 等 gameplay 系统

## 工作流

1. 对任何实现、修复、重构、蓝图、材质、资产或 gameplay 任务，先调用 ProjectGraph MCP tool `find_feature_context`，根据用户需求定位 owning system、相关模块、候选文件、资产和配置。
2. 必须随后调用 `get_context_pack`，获取精简上下文包。
3. 读取 `get_context_pack` 返回的文件和资源摘要；不要全仓库漫游。
4. 修改前必须说明：
   - owning system
   - related files
   - related blueprints
   - related assets
   - existing pattern to follow
   - validation plan
5. 按 context pack 显示的 owner 和既有架构实现最小改动。
6. 调用 `impact_analysis`，确认影响面、回归风险和需要验证的模块。
7. 运行验证：C++/Build/Target 优先编译 Editor Target；配置或脚本改动运行项目可用的 quick verify；蓝图、材质、资产修改必须通过 UE MCP / patch DSL 验证。

## 规则

- 修改代码、蓝图、材质或资产前必须先调用 `find_feature_context` 和 `get_context_pack`。
- 如果图谱缺失，先调用 `build_project_graph` 或 `update_project_graph`；如果 ProjectGraph MCP tools 仍不可用，停止并向用户说明阻塞原因，不要绕过图谱直接改代码。
- 不允许新建 Manager / Subsystem，除非 context pack 明确说明没有已有 owner，并且先说明现有 owner 为什么不适用。
- 不允许直接编辑 `.uasset`、`.umap` 或其他二进制资源。
- 不允许猜测蓝图、材质或资产内容；必须通过 ProjectGraph IR、UE MCP read/analyze 工具或 patch 工具确认。
- 蓝图、材质和资产修改必须走 `ue_editor` MCP tools、`ue5_agent_blueprint_tools` MCP server 或项目认可的 patch DSL；先 read/analyze，再 dry-run，再 apply，最后检查编译或工具返回结果。
- ProjectGraph 第一版只读，不实现复杂蓝图写入。
