# Agent ProjectGraph

- Agent ProjectGraph indexes source files, config, Build.cs data, and exported UE cache information into `.ai/graph`.
- The MCP server exposes graph build, update, feature context, context pack, query, asset reference, Blueprint usage, and impact analysis tools.
- Project Memory reads ProjectGraph through an adapter, with sqlite mode configured for this workspace.
- Graph results are evidence for owning systems and related files, but high-level implementation decisions should flow back through Project Memory context packs.
- Rebuild or update the graph after changes that add files, symbols, assets, or exported UE cache data.
