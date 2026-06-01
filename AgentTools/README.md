# AgentTools

Portable agent tooling for Unreal Engine projects.

## Tools

```text
AgentBlueprintTools/
  mcp-server/                      stdio MCP server that talks to the UE localhost bridge
  skills/ue5-blueprint-agent/      Codex skill for safe Blueprint/Material/Asset workflows
  docs/                            API, Patch DSL, architecture, troubleshooting
  examples/                        Patch DSL examples

AgentProjectGraph/
  ProjectGraph/                    TypeScript MCP server and graph indexer
  skills/ue-project-graph/         Codex skill for context discovery and impact analysis
```

The UE editor plugins stay in the target project's root `Plugins/` directory:

```text
Plugins/
  AgentBlueprintTools/
  AgentProjectGraph/
```

`AgentTools/` owns the portable agent-side runtime: MCP servers, skills, docs, examples, and one shared npm workspace.

## Migrating To Another UE Project

1. Copy these UE plugins into the target project root:

```text
Plugins/AgentBlueprintTools/
Plugins/AgentProjectGraph/
```

2. Copy `AgentTools/` into the target project root.
3. Enable `AgentBlueprintTools` and `AgentProjectGraph` in the target `.uproject` or via the editor plugin UI.
4. Install/build the shared Node workspace:

```powershell
cd AgentTools
npm install
npm run build:project-graph
```

5. Point Codex MCP config at:

```text
AgentTools/AgentBlueprintTools/mcp-server/server.js
AgentTools/AgentProjectGraph/ProjectGraph/dist/src/mcpServer.js
```

All current and future Node-based tools should use the shared `AgentTools/package.json` workspace and the single ignored `AgentTools/node_modules/` directory. The `.agents/skills` files in this project are thin wrappers. The portable skill sources live inside each tool directory.
