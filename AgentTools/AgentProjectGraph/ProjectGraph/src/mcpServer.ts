#!/usr/bin/env node
import { existsSync, readdirSync } from "node:fs";
import path from "node:path";
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import * as z from "zod/v4";
import { buildGraph } from "./commands/buildGraph.js";
import { findFeatureContext } from "./commands/findFeatureContext.js";
import { buildContextPack, type ContextPack } from "./graph/contextPack.js";
import { analyzeImpact } from "./graph/impactAnalysis.js";
import { GraphStore, toProjectPath } from "./graph/graphStore.js";
import { searchGraph } from "./graph/search.js";
import type { EdgeRecord, NodeRecord } from "./graph/schema.js";

const SERVER_INSTRUCTIONS = "Codex must call find_feature_context before implementation tasks in UE projects.";

function textResult(value: unknown) {
  return {
    content: [
      {
        type: "text" as const,
        text: typeof value === "string" ? value : JSON.stringify(value, null, 2)
      }
    ]
  };
}

function findUProjectRoot(start: string): string | undefined {
  let current = path.resolve(start);
  while (true) {
    try {
      if (readdirSync(current).some((entry) => entry.endsWith(".uproject"))) {
        return current;
      }
    } catch {
      return undefined;
    }

    const parent = path.dirname(current);
    if (parent === current) {
      return undefined;
    }
    current = parent;
  }
}

function projectRoot(): string {
  const envRoot = process.env.PROJECT_GRAPH_ROOT;
  if (envRoot) {
    return path.resolve(envRoot);
  }

  const fromCwd = findUProjectRoot(process.cwd());
  if (fromCwd) {
    return fromCwd;
  }

  const parent = path.dirname(process.cwd());
  const fromParent = findUProjectRoot(parent);
  if (fromParent) {
    return fromParent;
  }

  return process.cwd();
}

function graphDir(root = projectRoot()): string {
  return path.join(root, ".ai", "graph");
}

async function loadGraph(): Promise<GraphStore> {
  const root = projectRoot();
  const dir = graphDir(root);
  if (!existsSync(path.join(dir, "nodes.jsonl")) || !existsSync(path.join(dir, "edges.jsonl"))) {
    throw new Error(`ProjectGraph is not built at ${dir}. Run build_project_graph first.`);
  }
  return GraphStore.loadJsonl(dir);
}

function normalizeAssetPath(assetPath: string): string {
  const normalized = toProjectPath(assetPath).replace(/\\/g, "/");
  const slash = normalized.lastIndexOf("/");
  const dot = normalized.lastIndexOf(".");
  return dot > slash ? normalized.slice(0, dot) : normalized;
}

function nodeMatchesPath(node: NodeRecord, queryPath: string): boolean {
  const normalizedQuery = normalizeAssetPath(queryPath).toLowerCase();
  const normalizedNodePath = normalizeAssetPath(node.path || node.name).toLowerCase();
  return normalizedNodePath === normalizedQuery || normalizedNodePath.endsWith(normalizedQuery);
}

function resolveSeedNodes(store: GraphStore, seeds: string[]): { nodes: NodeRecord[]; warnings: string[] } {
  const nodes: NodeRecord[] = [];
  const warnings: string[] = [];
  for (const seed of seeds) {
    const direct = store.getNode(seed);
    if (direct) {
      nodes.push(direct);
      continue;
    }

    const byPath = store.nodes().find((node) => nodeMatchesPath(node, seed));
    if (byPath) {
      nodes.push(byPath);
      continue;
    }

    const search = searchGraph(store, seed, { limit: 1 })[0]?.node;
    if (search) {
      nodes.push(search);
      continue;
    }

    warnings.push(`Seed node not found: ${seed}`);
  }
  return { nodes: [...new Map(nodes.map((node) => [node.id, node])).values()], warnings };
}

function mergeContextPacks(packs: ContextPack[], warnings: string[], maxFiles: number, maxBlueprints: number, maxAssets: number) {
  const nodeMap = new Map<string, NodeRecord>();
  const edgeMap = new Map<string, EdgeRecord>();
  const fileMap = new Map<string, ContextPack["files"][number]>();

  for (const pack of packs) {
    for (const node of pack.nodes) nodeMap.set(node.id, node);
    for (const edge of pack.edges) edgeMap.set(`${edge.from}\0${edge.type}\0${edge.to}\0${JSON.stringify(edge.metadata)}`, edge);
    for (const file of pack.files) fileMap.set(file.path, file);
    warnings.push(...pack.warnings);
  }

  const nodes = [...nodeMap.values()];
  const files = [...fileMap.values()].slice(0, maxFiles);
  const blueprintsToRead = nodes
    .filter((node) => node.type === "Blueprint")
    .map((node) => node.path || node.name)
    .filter(Boolean)
    .slice(0, maxBlueprints);
  const assetsToInspect = nodes
    .filter((node) => ["Asset", "Material", "DataAsset", "Widget", "Level"].includes(node.type))
    .map((node) => node.path || node.name)
    .filter(Boolean)
    .slice(0, maxAssets);

  return {
    roots: packs.map((pack) => pack.root),
    nodes,
    edges: [...edgeMap.values()],
    files,
    filesToRead: files.map((file) => file.path),
    blueprintsToRead,
    assetsToInspect,
    importantSymbols: nodes
      .filter((node) => ["Module", "Class", "Struct", "Enum", "Function", "Property", "Variable", "Component", "Config"].includes(node.type))
      .map((node) => `${node.type}:${node.name}`),
    architectureRules: [
      "Codex must call find_feature_context before implementation tasks in UE projects.",
      "State the owning system before implementation.",
      "Do not directly edit .uasset or .umap files; use UE MCP / patch tools for asset edits."
    ],
    validationHints: [
      "Compile the target project's Editor target after C++ or Build.cs changes.",
      "Rebuild ProjectGraph after code or UE cache export changes.",
      "For Blueprint, material, or asset work, inspect UE MCP warnings and compile affected Blueprints."
    ],
    systems: [...new Set(nodes.map((node) => node.system).filter(Boolean))].sort(),
    warnings: [...new Set(warnings)]
  };
}

function referenceEdgesFor(store: GraphStore, nodeIds: Set<string>, direction: "in" | "out" | "both") {
  const referenceTypes = new Set(["REFERENCES_ASSET", "DEPENDS_ON", "INSTANCE_OF_MATERIAL", "PARENT_CLASS"]);
  return store.edges().filter((edge) => {
    if (!referenceTypes.has(edge.type)) return false;
    if (direction === "in") return nodeIds.has(edge.to);
    if (direction === "out") return nodeIds.has(edge.from);
    return nodeIds.has(edge.from) || nodeIds.has(edge.to);
  });
}

function impactWithDepth(store: GraphStore, nodeId: string, depth: number) {
  const root = store.getNode(nodeId);
  if (!root) {
    throw new Error(`Node not found: ${nodeId}`);
  }

  const selected = new Set<string>([nodeId]);
  let frontier = new Set<string>([nodeId]);
  const edges = new Map<string, EdgeRecord>();

  for (let currentDepth = 0; currentDepth < Math.max(1, depth); currentDepth += 1) {
    const next = new Set<string>();
    for (const id of frontier) {
      for (const edge of store.adjacent(id)) {
        edges.set(`${edge.from}\0${edge.type}\0${edge.to}\0${JSON.stringify(edge.metadata)}`, edge);
        const other = edge.from === id ? edge.to : edge.from;
        if (!selected.has(other) && store.getNode(other)) {
          selected.add(other);
          next.add(other);
        }
      }
    }
    frontier = next;
    if (frontier.size === 0) break;
  }

  return {
    ...analyzeImpact(store, nodeId),
    depth,
    nodes: [...selected].map((id) => store.getNode(id)).filter(Boolean),
    edges: [...edges.values()]
  };
}

const server = new McpServer(
  {
    name: "ue-project-graph",
    version: "0.1.0"
  },
  {
    instructions: SERVER_INSTRUCTIONS
  }
);

server.registerTool(
  "build_project_graph",
  {
    description: "Build the local UE ProjectGraph. Writes .ai/graph only; does not edit Unreal assets.",
    inputSchema: z.object({
      mode: z.enum(["full", "incremental"]),
      includeUECache: z.boolean()
    })
  },
  async ({ mode, includeUECache }) => {
    const root = projectRoot();
    const result = await buildGraph({ projectRoot: root, includeUeCache: includeUECache });
    return textResult({
      ...result,
      requestedMode: mode,
      actualMode: "full",
      instructions: SERVER_INSTRUCTIONS
    });
  }
);

server.registerTool(
  "update_project_graph",
  {
    description: "Update ProjectGraph after file changes. Current MVP performs a safe full rebuild of .ai/graph.",
    inputSchema: z.object({
      changedPaths: z.array(z.string())
    })
  },
  async ({ changedPaths }) => {
    const root = projectRoot();
    const result = await buildGraph({ projectRoot: root, includeUeCache: true });
    return textResult({
      ...result,
      changedPaths,
      actualMode: "full-rebuild"
    });
  }
);

server.registerTool(
  "find_feature_context",
  {
    description: "Find owning system and context candidates for a UE feature, bug, refactor, or implementation task.",
    inputSchema: z.object({
      query: z.string(),
      maxResults: z.number().int().min(1).max(100)
    })
  },
  async ({ query, maxResults }) => textResult(await findFeatureContext(query, { projectRoot: projectRoot(), limit: maxResults }))
);

server.registerTool(
  "get_context_pack",
  {
    description: "Return a compact context pack around seed nodes. Read-only.",
    inputSchema: z.object({
      seedNodes: z.array(z.string()).min(1),
      maxFiles: z.number().int().min(0).max(200),
      maxBlueprints: z.number().int().min(0).max(200),
      maxAssets: z.number().int().min(0).max(500)
    })
  },
  async ({ seedNodes, maxFiles, maxBlueprints, maxAssets }) => {
    const store = await loadGraph();
    const resolved = resolveSeedNodes(store, seedNodes);
    const packs = [];
    for (const node of resolved.nodes) {
      packs.push(
        await buildContextPack(store, node.id, {
          projectRoot: projectRoot(),
          depth: 1,
          maxNodes: Math.max(20, maxFiles + maxBlueprints + maxAssets + 10)
        })
      );
    }
    return textResult(mergeContextPacks(packs, resolved.warnings, maxFiles, maxBlueprints, maxAssets));
  }
);

server.registerTool(
  "query_graph",
  {
    description: "Search ProjectGraph nodes. Read-only.",
    inputSchema: z.object({
      query: z.string(),
      types: z.array(z.string()).optional()
    })
  },
  async ({ query, types }) => {
    const store = await loadGraph();
    return textResult({
      query,
      results: searchGraph(store, query, { types, limit: 50 })
    });
  }
);

server.registerTool(
  "find_asset_references",
  {
    description: "Find graph references for an asset path. Read-only.",
    inputSchema: z.object({
      assetPath: z.string(),
      direction: z.enum(["in", "out", "both"])
    })
  },
  async ({ assetPath, direction }) => {
    const store = await loadGraph();
    const matches = store.nodes().filter((node) => nodeMatchesPath(node, assetPath));
    const nodeIds = new Set(matches.map((node) => node.id));
    const edges = referenceEdgesFor(store, nodeIds, direction);
    const relatedNodeIds = new Set(edges.flatMap((edge) => [edge.from, edge.to]));
    return textResult({
      assetPath,
      direction,
      matchedNodes: matches,
      references: edges,
      relatedNodes: [...relatedNodeIds].map((id) => store.getNode(id)).filter(Boolean)
    });
  }
);

server.registerTool(
  "find_blueprint_usage",
  {
    description: "Find references and usage around a Blueprint path. Read-only.",
    inputSchema: z.object({
      blueprintPath: z.string()
    })
  },
  async ({ blueprintPath }) => {
    const store = await loadGraph();
    const matches = store.nodes().filter((node) => node.type === "Blueprint" && nodeMatchesPath(node, blueprintPath));
    const nodeIds = new Set(matches.map((node) => node.id));
    const edges = store.edges().filter((edge) => nodeIds.has(edge.from) || nodeIds.has(edge.to));
    const relatedNodeIds = new Set(edges.flatMap((edge) => [edge.from, edge.to]));
    return textResult({
      blueprintPath,
      matchedBlueprints: matches,
      usages: edges,
      relatedNodes: [...relatedNodeIds].map((id) => store.getNode(id)).filter(Boolean)
    });
  }
);

server.registerTool(
  "impact_analysis",
  {
    description: "Analyze local graph impact around a node id. Read-only.",
    inputSchema: z.object({
      nodeId: z.string(),
      depth: z.number().int().min(1).max(5)
    })
  },
  async ({ nodeId, depth }) => {
    const store = await loadGraph();
    return textResult(impactWithDepth(store, nodeId, depth));
  }
);

async function main(): Promise<void> {
  const transport = new StdioServerTransport();
  await server.connect(transport);
}

main().catch((error: unknown) => {
  console.error(error instanceof Error ? error.message : String(error));
  process.exit(1);
});
