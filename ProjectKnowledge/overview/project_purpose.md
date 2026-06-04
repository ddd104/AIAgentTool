# AIAgentTool Project Purpose

- The workspace hosts a UE5 project together with local agent tooling for safe Unreal Editor automation.
- `AgentTools/` owns the portable agent-side runtimes, including Project Memory, ProjectGraph, and Blueprint MCP servers.
- `Plugins/AgentBlueprintTools/` and `Plugins/AgentProjectGraph/` contain the UE Editor plugins that provide bridge and graph data.
- Agent workflows must use structured MCP tools and generated context instead of directly editing Unreal binary assets.
- Project Memory is the first context entry point for implementation, debugging, refactoring, and architecture tasks.
