---
name: ue-project-graph
description: 当任务涉及 UE C++、蓝图、资产、材质、配置、新功能、bug 修复、重构、UI 功能、输入系统、AI/Combat/Inventory/Interaction 等 gameplay 系统时触发；必须先用 ProjectGraph MCP 定位上下文再实施
allow_implicit_invocation: true
---

# UE Project Graph Skill

Canonical skill source lives at `AgentTools/AgentProjectGraph/skills/ue-project-graph/SKILL.md`.

Use the `ue_project_graph` MCP server before implementation work:

1. Call `find_feature_context`.
2. Call `get_context_pack`.
3. State owning system, related files, related blueprints/assets, existing pattern, and validation plan.
4. After changes, call `impact_analysis` and run the relevant validation.

Do not directly edit `.uasset` or `.umap` files.
