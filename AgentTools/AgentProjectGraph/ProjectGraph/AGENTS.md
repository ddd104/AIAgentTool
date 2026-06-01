# ProjectGraph AGENTS.md

This directory contains the portable TypeScript ProjectGraph MCP server and indexers.

## Scope

- Build or update the read-only graph under `.ai/graph`.
- Query UE project context through `find_feature_context`, `get_context_pack`, `query_graph`, and `impact_analysis`.
- Do not edit `.uasset`, `.umap`, or generated graph output unless explicitly requested.

## Workflow

1. Run `npm install` and `npm run build:project-graph` from `AgentTools` when dependencies or TypeScript sources change.
2. Use `build_project_graph` or `update_project_graph` to refresh `.ai/graph`.
3. Use `find_feature_context` before implementation work.
4. Use `get_context_pack` before reading broad project files.
5. Use `impact_analysis` after code or configuration changes.

## Validation

```powershell
cd AgentTools
npm run build:project-graph
npm run test:project-graph
```

For UE plugin changes, compile the target project's Editor target after placing plugins under the root `Plugins/` directory.
