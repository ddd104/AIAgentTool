---
name: ue5-blueprint-agent
description: Use this skill when modifying Unreal Engine 5 Blueprints, Blueprint variables, Blueprint functions, Materials, Material parameters, or Content Browser assets through the local UE5 MCP bridge.
---

# UE5 Blueprint Agent Skill

Canonical skill source lives at `AgentTools/AgentBlueprintTools/skills/ue5-blueprint-agent/SKILL.md`.

Use the `ue5_agent_blueprint_tools` MCP server. Never edit `.uasset`, `.umap`, or other binary Unreal assets directly.

Blueprint/material/asset writes must follow the canonical flow:

1. Read first (`read_blueprint`, `analyze_blueprint_graph`, or `read_material`).
2. Produce a minimal patch.
3. Run dry-run.
4. Apply only after dry-run succeeds.
5. Inspect compile/tool results.
6. Leave `saveOnSuccess`, `saveAssets`, and `saveLevel` false unless explicitly requested.
