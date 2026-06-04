# Architecture Rules

- Project Memory must run before implementation, bug fix, refactor, UE C++, Blueprint, Material, asset, UI, input, AI, combat, inventory, interaction, system design, or architecture work.
- ProjectGraph is a lower-level graph source; prefer Project Memory context packs unless graph details or graph rebuilds are specifically needed.
- Never directly edit `.uasset`, `.umap`, or other Unreal binary resource files.
- Blueprint, Material, asset, and level actor changes must go through UE MCP tools or an approved patch DSL with read/analyze, dry-run, apply, and validation steps.
- `AgentTools/` contains agent-side runtimes; UE Editor plugin code remains under `Plugins/AgentBlueprintTools/` and `Plugins/AgentProjectGraph/`.
- Do not create new managers, subsystems, or global framework layers unless context evidence shows no suitable existing owner.
