# Project Memory System

Project Memory System 是独立于 Project Graph 的项目记忆工具。它读取项目资料、文档、表格、设计说明和图谱 adapter 结果，生成可检索的项目记忆缓存。

它不直接编辑 UE 项目源码，也不直接编辑 `.uasset`、`.umap` 或其他二进制资源。

## 当前能力

- TypeScript CLI：`memory status`、`memory build`、`memory update`、`memory query <text>`、`memory context-pack <query>`。
- 文档索引：`ProjectMemory.project.json` 中的 `knowledgeRoots`，输出 `.ai/project-memory/index/documents.jsonl` 和 `.ai/project-memory/index/fts.sqlite`。
- 摘要缓存：project capsule、architecture rules、system map、patterns、validation memory。
- Implementation Memory：缓存已实现功能、现有实现模式、可复用符号、蓝图/资产链接和扩展禁忌，输出 `.ai/project-memory/runs/<runId>/implementation_map.json`。
- ProjectGraph adapter：支持 `graph.enabled=false`、`sqlite`、`mcp` 配置模式；Context Pack 只依赖 `ProjectGraphClient` 接口。
- MCP Server：STDIO transport，暴露 tools、resources 和 prompts。

## 安装与验证

```powershell
cd AgentTools/ProjectMemorySystem
npm install
npm test
npm run build
node dist/src/cli.js memory status --project ../..
node dist/src/cli.js memory context-pack "待查询内容" --project ../..
```

## CLI

```powershell
node dist/src/cli.js memory status --project ../..
node dist/src/cli.js memory build --project ../..
node dist/src/cli.js memory build --docs-only --project ../..
node dist/src/cli.js memory build --capsule --project ../..
node dist/src/cli.js memory build --systems --project ../..
node dist/src/cli.js memory build --patterns --project ../..
node dist/src/cli.js memory update --project ../..
node dist/src/cli.js memory query "keyword" --project ../..
node dist/src/cli.js memory context-pack "{\"query\":\"enemy health bar damage UI\",\"taskType\":\"feature\"}" --project ../..
```

完整 `memory build` 会按顺序生成文档索引、项目摘要、系统摘要、模式库、validation memory、UE cache summaries 和 Implementation Memory。`memory update` 会刷新变化文档和最新 Implementation Memory 指针。

## Implementation Memory

Implementation Memory 用于回答“项目里已经怎么做过”和“这次应复用什么”。它只读取证据来源，不调用 LLM API，不直接读取或编辑 `.uasset`。

主要数据来源：

- `ProjectKnowledge/systems/*.md`
- `ProjectKnowledge/patterns/*.md`
- `ProjectKnowledge/design/*.md`
- `.ai/project-memory/ue-cache/summaries.jsonl`
- `ProjectGraphClient` adapter 返回的文件、蓝图、资产和符号节点

输出文件：

```text
.ai/project-memory/runs/<runId>/implementation_map.json
.ai/project-memory/runs/latest.json
```

Context Pack 会合并这些字段：

- `relevantImplementations`
- `existingPatterns`
- `reusableSymbols`
- `reusableBlueprints`
- `reusableAssets`
- `forbiddenApproaches`
- `extensionGuidance`

证据不足时不会编造项目事实，而是写入 `missingInformation`。

## MCP Server

构建后使用 STDIO 启动：

```powershell
node AgentTools/ProjectMemorySystem/dist/src/mcpServer.js
```

可通过环境变量指定项目根和配置文件：

```powershell
$env:PROJECT_MEMORY_ROOT="F:\UE5Project\qirui_v25"
$env:PROJECT_MEMORY_CONFIG="F:\UE5Project\qirui_v25\AgentTools\ProjectMemorySystem\ProjectMemory.project.json"
node AgentTools/ProjectMemorySystem/dist/src/mcpServer.js
```

### Tools

- `memory_status`
- `build_project_memory`
- `update_project_memory`
- `query_project_memory`
- `get_project_capsule`
- `find_owning_system`
- `find_existing_patterns`
- `get_context_pack`
- `impact_analysis`

### Resources

- `project://capsule`
- `project://architecture-rules`
- `project://system-map`
- `project://system/{name}`
- `project://patterns`
- `project://pattern/{name}`
- `project://validation`
- `project://summary/{kind}/{id}`
- `project://ue-cache`

所有 resources 都从 `.ai/project-memory` 读取缓存。缓存缺失时先运行 `memory build` 或调用 MCP tool `build_project_memory`。

### Prompts

- `implement_feature_with_project_memory`
- `debug_with_project_memory`
- `refactor_with_project_memory`
- `modify_blueprint_with_project_memory`

这些 prompt 返回固定工作流模板：先 `memory_status`，stale 时先 `update_project_memory`，再 `get_context_pack`，修改前说明 owning system、现有模式、复用对象和验证计划；禁止直接编辑 `.uasset`。

## .codex/config.toml 示例

在 Codex 的 `.codex/config.toml` 中添加 MCP server。`args` 可以使用绝对路径；`ProjectMemory.project.json` 内部仍必须使用相对于项目根的路径。

```toml
[mcp_servers.project_memory]
command = "node"
args = ["F:/UE5Project/qirui_v25/AgentTools/ProjectMemorySystem/dist/src/mcpServer.js"]
cwd = "F:/UE5Project/qirui_v25"

[mcp_servers.project_memory.env]
PROJECT_MEMORY_ROOT = "F:/UE5Project/qirui_v25"
PROJECT_MEMORY_CONFIG = "F:/UE5Project/qirui_v25/AgentTools/ProjectMemorySystem/ProjectMemory.project.json"
```

迁移到其他项目时，只需要复制 `AgentTools/ProjectMemorySystem/`，调整目标项目内的 `ProjectMemory.project.json`，再把 `.codex/config.toml` 中的路径改成新项目位置。

## 配置

默认配置文件：

```text
AgentTools/ProjectMemorySystem/ProjectMemory.project.json
```

所有配置路径都应相对于项目根目录。关键字段：

- `projectId`
- `knowledgeRoots`
- `memoryOutputRoot`
- `graph.enabled`
- `graph.mode`

`graph.mode` 支持：

- `sqlite`
- `mcp`

## 与 Project Graph 的边界

Project Graph 负责代码、蓝图和资产关系图谱。Project Memory System 负责资料读取、摘要、索引、检索和上下文包。Project Memory System 可以通过 `ProjectGraphClient` adapter 读取图谱结果，但不能强依赖某个具体图谱实现。

## Skill 相关说明

如果后续新增 Codex skill，skill 文档应使用中文描述触发条件、读取顺序、禁止事项和验证步骤。涉及 UE 蓝图、材质、资产修改时，仍必须遵守项目门禁：先读取图谱上下文，再通过 UE MCP 或项目认可的 patch 工具执行，不直接编辑二进制资源。
