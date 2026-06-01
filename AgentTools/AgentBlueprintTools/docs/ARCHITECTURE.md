# Architecture

## Overview

```text
Codex / Agent
  │ stdio MCP
  ▼
mcp-server/server.js
  │ HTTP localhost + x-abt-token
  ▼
UE5 Editor Plugin: AgentBlueprintTools
  │ Editor APIs on game thread
  ▼
Blueprint / Material / Asset
```

## Why not edit .uasset directly?

`.uasset` is not a stable source format. Agent edits must be expressed as structured intent, then executed by Unreal Editor APIs.

## Main boundaries

| Layer | Responsibility |
|---|---|
| Codex / LLM | Understand task, choose tools, produce patch DSL |
| MCP server | Schema, YAML parsing, bridge calls |
| UE bridge | Local HTTP routes, auth token, game-thread dispatch |
| UE tool layer | Read IR, validate patch, create nodes, connect pins, compile |

## Safety model

- Localhost only.
- Token required for all bridge calls.
- Dry-run before write.
- Transaction wrapper around writes.
- Compile after Blueprint changes.
- Save only when requested.
