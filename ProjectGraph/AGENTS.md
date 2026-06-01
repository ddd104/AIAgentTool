# ProjectGraph AGENTS.md

本目录用于放置可移植的 UE Project Graph 工具、索引、查询入口和相关说明。规则面向后续所有维护 ProjectGraph 或依赖 ProjectGraph 定位项目上下文的 AI 代理。

## UE 项目概览

- UE 项目：`qirui_v25.uproject`
- 开发平台：Windows
- 目标平台：Android
- 引擎关联：`UEAS`
- 当前确认版本：Unreal Engine 5.6.1
- 主业务模块：`Source/qirui_v25`
- 主要项目插件：`Plugins/HMICore`、`Plugins/HMIEnvironment`、`Plugins/SRRender`、`Plugins/AgentBlueprintTools`
- ProjectGraph 目标：只读扫描 C++、Config、Build.cs、Target.cs、Script、蓝图、材质和资产摘要，帮助 Codex 定位 owning system、影响面和最小文件集合。

## 强制规则

- 不允许直接编辑 `.uasset`、`.umap` 或其他二进制资源文件。
- 修改 C++、Config、Script、Build.cs、Target.cs 或插件代码前，必须先调用 ProjectGraph MCP tools 定位上下文。
- 功能实现前必须先说明 owning system，例如 `qirui_v25`、`HMICore/HMIFramework`、`HMIEnvironment`、`SRRender`、`AgentBlueprintTools` 或 `ProjectGraph`。
- 蓝图、材质、Content Browser asset 或关卡 Actor 修改必须走 UE MCP / patch 工具；修改前先读取资源，应用前先 dry-run，完成后检查编译或工具返回结果。
- 第一版 ProjectGraph 只做只读图谱，不实现复杂蓝图写入。
- 只读取图谱工具、配方、日志或错误结果明确指向的最小文件集合。
- 不把生成图谱输出提交为源码，除非用户明确要求。

## ProjectGraph 工作流

1. 调用 `find_feature_context`，用用户需求定位 owning system、候选模块、资产和配置。
2. 调用 `get_context_pack`，获取最小上下文包。
3. 读取上下文包返回的文件和资源摘要。
4. 明确 owning system 和预期涉及文件。
5. 按原架构做最小实现，不做无关重构。
6. 调用 `impact_analysis` 检查影响面。
7. 运行对应验证命令。

## 构建命令

Editor Target 编译：

```powershell
& "$env:UE_ROOT\Engine\Build\BatchFiles\Build.bat" qirui_v25Editor Win64 Development -Project="F:\UE5Project\qirui_v25\qirui_v25.uproject" -WaitMutex -FromMsBuild
```

如果 `UE_ROOT` 未设置，可使用当前 `UEAS` 映射路径：

```powershell
& "F:\EPIC\Engine\Windows\Engine\Build\BatchFiles\Build.bat" qirui_v25Editor Win64 Development -Project="F:\UE5Project\qirui_v25\qirui_v25.uproject" -WaitMutex -FromMsBuild
```

Android 打包默认入口：

```powershell
Set-Location F:\UE5Project\qirui_v25\Tools\Build
.\HMI_Release_WithLog.bat
```

## 测试命令

优先快速验证：

```powershell
python tools/verify_change.py --quick
```

如果该脚本不存在，使用项目内可用验证：

```powershell
python AITools/verify_change.py --quick
```

ProjectGraph CLI 预期验证入口：

```powershell
python ProjectGraph/project_graph/cli.py build --project F:\UE5Project\qirui_v25\qirui_v25.uproject
python ProjectGraph/project_graph/cli.py query "用户需求关键词"
```

MCP 配置验证：

```powershell
node mcp-server/server.js
```

## 图数据时效性规则

请勿在执行每项任务之前都重建完整的项目图（ProjectGraph）。

请遵循以下策略：

1. 如果 `.ai/graph/project_graph.sqlite` 文件缺失，则以 `full` 模式调用 `build_project_graph`。
2. 否则，调用 `graph_status`。
3. 如果 `graph_status.recommendedAction` 的值为 `none`，则继续执行。
4. 如果其值为 `incremental_update`，则调用 `update_project_graph`。
5. 如果其值为 `refresh_ue_cache`，则针对发生变更的资源调用 `refresh_ue_cache`，随后调用 `update_project_graph`。
6. 如果其值为 `full_rebuild`，则在以 `full` 模式运行 `build_project_graph` 之前，需先征得批准。
7. 在编辑文件后，针对发生变更的路径调用 `update_project_graph`。
8. 在编辑蓝图（Blueprints）、材质或资源（Assets）后，仅对这些特定的资源进行刷新。

## 编码规范

- 默认使用 ASCII；只有既有文件或业务文本需要中文时才使用非 ASCII。
- Python 代码优先使用标准库，路径处理使用 `pathlib`，JSON 输出保持稳定 schema。
- C++ 代码遵循 UE 风格：`F`/`U`/`A`/`I` 前缀、`TEXT()` 字符串、`FPaths`/`IFileManager` 处理路径。
- 扫描器优先只读，默认跳过 `Binaries/`、`Intermediate/`、`DerivedDataCache/`、`Saved/`、`ArchivedBuilds/`、`Releases/`、`Build/`、`.vs/`、`.idea/`。
- 对不完整或正则推断的图谱结果必须标记 `confidence` 或 `source`，不要伪装成完整编译级索引。
- 变更保持最小范围，不做无关格式化，不回滚用户已有修改。
