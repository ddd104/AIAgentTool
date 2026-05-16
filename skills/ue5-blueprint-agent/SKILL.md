---
name: ue5-blueprint-agent
description: Use this skill when modifying Unreal Engine 5 Blueprints, Blueprint variables, Blueprint functions, Materials, Material parameters, or Content Browser assets through the local UE5 MCP bridge.
---

# UE5 Blueprint Agent Skill

Use the `ue5_agent_blueprint_tools` MCP server. Never edit `.uasset` files directly.

## Blueprint read flow

1. Call `read_blueprint(assetPath)`.
2. Call `analyze_blueprint_graph(assetPath, graphName)` if the task mentions a specific graph or function.
3. Summarize execution paths, variables read/written, components touched, and external function calls.

## Blueprint write flow

1. Call `read_blueprint` first.
2. If a function call is needed, call `list_callable_functions(query)`.
3. Produce a minimal YAML patch.
4. Call `dry_run_blueprint_patch`.
5. Only if dry-run succeeds, call `apply_blueprint_patch`.
6. Inspect compile result.
7. If compile fails, report the log and do not save.

## Patch principles

- Use small functions.
- Prefer explicit `ensure_variable` and `ensure_function` before adding nodes.
- Do not invent pin names.
- Keep graph edits small and reversible.
- Avoid Tick unless requested.

## Supported Blueprint patch ops

- `ensure_variable`
- `ensure_function`
- `ensure_move_function`
- `ensure_component`
- `set_static_mesh`
- `set_component_material`
- `add_node`
- `set_pin_default`
- `connect_exec`
- `connect_data`
- `remove_node`
- `set_blueprint_property`

## Material flow

1. Call `read_material` first.
2. Use `patch_material` with a small material patch.
3. Recompile material after edits.

## Asset flow

Use `create_asset`, `read_asset`, `set_asset_property`, `save_asset`, and `delete_asset` rather than raw file writes. Only delete known temporary assets or assets the user explicitly asked to remove.

Use `place_actor` to place an Actor Blueprint or Actor class in the current editor level. Leave `saveLevel` false unless the user asks to save the level.

## JSON audit trail

Bridge requests and responses are written under `Saved/AgentBlueprintTools/Requests/` when `ABT_AUDIT_JSON` is unset or truthy. Set `ABT_AUDIT_JSON=0` to disable this local audit trail.
