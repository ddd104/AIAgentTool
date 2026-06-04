# Agent Blueprint Tools

- Agent Blueprint Tools provides a local stdio MCP server and a UE Editor-only plugin bridge for Blueprint, Material, asset, performance, and level actor workflows.
- The MCP server forwards requests to the UE bridge at `http://127.0.0.1:31055` by default.
- The UE bridge is started by the `AgentBlueprintTools` editor module during Unreal Editor startup, except commandlet runs.
- The bridge requires the `x-abt-token` header; this project uses `change-me-local` unless `ABT_BRIDGE_TOKEN` overrides it.
- Blueprint edits must read the asset first, analyze graph detail when needed, dry-run the patch, apply only after dry-run succeeds, and compile afterward.
- Save operations remain explicit; default tool options should avoid saving assets or levels unless the task requires it.
