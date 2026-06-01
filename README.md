# AIAgentTool

This project keeps Unreal editor plugins in the normal project-level `Plugins/` directory, and keeps reusable agent-side tooling under `AgentTools/` so the support tools can be copied into another Unreal project with a small amount of integration work.

## Layout

```text
Plugins/
  AgentBlueprintTools/   UE Editor plugin for Blueprint/Material/Asset editing APIs
  AgentProjectGraph/     UE Editor plugin for read-only project graph exports

AgentTools/
  package.json           Shared npm workspace for all agent tools
  AgentBlueprintTools/   Blueprint/Material/Asset MCP server, docs, examples, skill
  AgentProjectGraph/     TypeScript graph MCP/indexer, skill
```

For migration details, start with `AgentTools/README.md`.
