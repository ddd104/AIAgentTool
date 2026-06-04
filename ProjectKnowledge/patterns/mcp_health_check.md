# MCP Health Check Pattern

- Confirm MCP server startup independently before debugging tool behavior inside Codex.
- Use `tools/list` or the MCP inspector to verify that expected tools are exposed by the server.
- For Project Memory, call `memory_status` first; if status is missing or stale, update or build before calling `get_context_pack`.
- For ProjectGraph, call `find_feature_context` and then `get_context_pack` for implementation or bug-fix work.
- For UE bridge tools, call `ping_ue_bridge` before Blueprint, Material, asset, performance, or actor placement operations.
- If `ping_ue_bridge` fails with a connection error, check that Unreal Editor is open, `AgentBlueprintTools` is enabled, the bridge port matches `ABT_BRIDGE_URL`, and the token matches `ABT_BRIDGE_TOKEN`.
