# UE5 Agent Blueprint Tools

这是一个 **MCP + UE5 C++ Editor Plugin** 的完整参考骨架，用来让 Codex / LLM agent 读取、分析、修改 UE5 Blueprint、Material 和基础资产。

它的目标不是让模型直接编辑 `.uasset`。正确工作流是：

```text
Codex / Agent
  -> local stdio MCP server
  -> localhost UE bridge
  -> UE5 C++ Editor Plugin
  -> Blueprint / Material / Asset APIs
```

## 包含内容

```text
mcp-server/                         Node.js stdio MCP server
Plugins/AgentBlueprintTools/         UE5 Editor-only C++ plugin
examples/patches/                    Blueprint / Material / Asset patch 示例
skills/ue5-blueprint-agent/          Codex Skill
.codex/config.toml                   Codex MCP 配置示例
AGENTS.md                            项目级 agent 规则
docs/                                架构、DSL、API、排错文档
```

## 重要声明

- 这是源码参考实现，不是预编译插件。
- 没有直接写 `.uasset`。所有写入通过 UE Editor API。
- Blueprint 写入能力刻意收敛在最小闭环：变量、函数、事件、Branch、Get/Set Variable、CallFunction、pin 连接、默认值、compile。
- 不同 UE5 小版本的 Editor API 可能有轻微差异，第一次编译可能需要按实际 API 做小修。

## 快速安装

### 1. 放入 UE 项目

把插件复制到你的项目：

```text
YourProject/
  Plugins/
    AgentBlueprintTools/
```

如果你的项目是纯蓝图项目，先创建一个空 C++ 类，让项目变成 C++ 项目。

### 2. 编译插件

在 Windows 上建议：

1. 右键 `.uproject`，Generate Visual Studio project files。
2. 用 Visual Studio 打开 `.sln`。
3. 编译 `Development Editor`。
4. 启动 Unreal Editor。
5. Edit -> Plugins，确认 `AgentBlueprintTools` 已启用。

### 3. 配置 bridge token

插件默认监听：

```text
http://127.0.0.1:31055
```

环境变量：

```powershell
$env:ABT_BRIDGE_TOKEN="change-me-local"
$env:ABT_BRIDGE_PORT="31055"
```

### 4. 安装 MCP server

```bash
cd mcp-server
npm install
node server.js
```

### 5. Codex 配置

项目根目录已经包含：

```text
.codex/config.toml
```

你也可以用 Codex CLI 添加：

```bash
codex mcp add ue5_agent_blueprint_tools -- node mcp-server/server.js
```

## 最小工作流

```text
1. read_blueprint
2. analyze_blueprint_graph
3. list_callable_functions, 如果要调用函数
4. dry_run_blueprint_patch
5. apply_blueprint_patch
6. compile_blueprint
```

## 示例 patch

```yaml
target: /Game/Blueprints/BP_Door
operations:
  - op: ensure_looping_move
    function: MoveByDelta
    interval: 0.25
    delta: [25, 0, 0]
```

`ensure_looping_move` 会创建精简函数并在 `BeginPlay` 中用 Timer 循环调用，避免让 agent 手写多节点连接，也避免默认依赖 Tick。

## 调试

检查 UE bridge：

```powershell
Invoke-RestMethod `
  -Method Get `
  -Uri http://127.0.0.1:31055/v1/health `
  -Headers @{ "x-abt-token" = "change-me-local" }
```

MCP inspector：

```bash
cd mcp-server
npm run inspect
```

## 当前支持的 MCP tools

- `ping_ue_bridge`
- `read_blueprint`
- `analyze_blueprint_graph`
- `list_callable_functions`
- `dry_run_blueprint_patch`
- `apply_blueprint_patch`
- `compile_blueprint`
- `read_material`
- `patch_material`
- `analyze_performance`
- `dry_run_performance_optimization`
- `apply_performance_optimization`
- `create_asset`
- `read_asset`
- `set_asset_property`
- `save_asset`
- `delete_asset`
- `place_actor`

`read_blueprint` 默认返回 compact summary。只有需要检查节点和 pin 时，才使用 `mode: "full"`。

## 生产建议

真正接入生产项目时，请补上：

- 资产路径白名单
- 函数调用白名单
- patch 审计日志
- per-project schema cache
- 更细的 rollback 策略
- 自动化测试覆盖
- 对 Timeline、Delegate、Interface、Macro、Animation Blueprint 的分阶段支持

别让 agent 拿到“任意 Editor API 执行权”。这不是自由，这是事故排队系统。
