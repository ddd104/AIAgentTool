# Project Memory System

- Project Memory System reads project knowledge, configured source roots, UE cache summaries, and ProjectGraph adapter results.
- The MCP server exposes `memory_status`, `build_project_memory`, `update_project_memory`, `query_project_memory`, `get_context_pack`, and impact-related tools.
- The system writes cache artifacts under `.ai/project-memory` and does not directly edit Unreal assets.
- `AgentTools/ProjectMemorySystem/ProjectMemory.project.json` configures knowledge roots, memory output, source policy, graph mode, and validation inputs.
- Context packs combine owning system, relevant files, architecture rules, patterns, reusable symbols, UE cache summaries, and missing information.
