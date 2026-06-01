# UE ProjectGraph

Local, read-only graph tooling for this Unreal Engine project.

## MCP Server

Build the TypeScript project first:

```powershell
cd F:\UE5Project\qirui_v25\ProjectGraph
npm install
npm run build
```

Enable the MCP server from the repo root in `.codex/config.toml`:

```toml
[mcp_servers.ue_project_graph]
command = "node"
args = ["ProjectGraph/dist/src/mcpServer.js"]
cwd = "."
enabled = true
startup_timeout_sec = 20
tool_timeout_sec = 180
env = { PROJECT_GRAPH_ROOT = "." }
```

Server instruction:

```text
Codex must call find_feature_context before implementation tasks in UE projects.
```

The server uses STDIO transport and exposes:

- `build_project_graph`
- `update_project_graph`
- `find_feature_context`
- `get_context_pack`
- `query_graph`
- `find_asset_references`
- `find_blueprint_usage`
- `impact_analysis`

Only `build_project_graph` and `update_project_graph` write `.ai/graph`. Query tools are read-only and never modify `.uasset` or `.umap` files.
