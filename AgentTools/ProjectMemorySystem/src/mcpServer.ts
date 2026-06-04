#!/usr/bin/env node
import { existsSync, readdirSync } from "node:fs";
import { readFile, readdir } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { McpServer, ResourceTemplate } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import * as z from "zod/v4";
import { buildMemory } from "./commands/buildMemory.js";
import { getContextPack } from "./commands/getContextPack.js";
import { memoryStatus } from "./commands/memoryStatus.js";
import { updateMemory } from "./commands/updateMemory.js";
import { loadProjectConfig, type LoadProjectConfigOptions } from "./config/loadProjectConfig.js";
import { createProjectGraphClient } from "./graph/projectGraphClient.js";
import { FullTextIndex } from "./index/fullTextIndex.js";
import { findExistingPatterns } from "./retrieval/findExistingPatterns.js";
import { buildMemoryContextPack } from "./retrieval/contextPackBuilder.js";
import type { ContextTaskType, EvidenceItem } from "./types/contextPackTypes.js";
import type { LoadedProjectConfig, MemorySearchResult } from "./types/memoryTypes.js";

export const SERVER_INSTRUCTIONS = [
  "Codex 在实现 UE 功能前必须先调用 memory_status。",
  "如果 Project Memory 状态 stale、missing 或 needs_full_rebuild，先调用 update_project_memory；需要完整重建时调用 build_project_memory。",
  "然后调用 get_context_pack 获取项目记忆上下文。",
  "修改前必须说明 owning system、现有模式、可复用变量/类/蓝图/资产、验证计划。",
  "不要直接编辑 .uasset、.umap 或其他 Unreal 二进制资源。"
].join("\n");

const contextTaskTypes = ["feature", "bugfix", "refactor", "blueprint", "material", "asset", "question"] as const;

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

function promptResult(description: string, text: string) {
  return {
    description,
    messages: [
      {
        role: "user" as const,
        content: {
          type: "text" as const,
          text
        }
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
    if (parent === current) return undefined;
    current = parent;
  }
}

function projectOptions(): LoadProjectConfigOptions {
  const envRoot = process.env.PROJECT_MEMORY_ROOT;
  const envConfig = process.env.PROJECT_MEMORY_CONFIG;
  const projectRoot = envRoot ? path.resolve(envRoot) : findUProjectRoot(process.cwd());
  return {
    projectRoot,
    configPath: envConfig ? path.resolve(envConfig) : undefined
  };
}

async function loadMemoryConfig(): Promise<LoadedProjectConfig> {
  return loadProjectConfig(projectOptions());
}

function mimeTypeFor(filePath: string): string {
  const extension = path.extname(filePath).toLowerCase();
  if (extension === ".md") return "text/markdown";
  if (extension === ".json") return "application/json";
  if (extension === ".jsonl") return "application/jsonl";
  return "text/plain";
}

function cachePath(loaded: LoadedProjectConfig, relativePath: string): string {
  const resolved = path.resolve(loaded.resolvedMemoryOutputRoot, relativePath);
  const root = path.resolve(loaded.resolvedMemoryOutputRoot);
  if (resolved !== root && !resolved.startsWith(`${root}${path.sep}`)) {
    throw new Error(`Refusing to read outside memoryOutputRoot: ${relativePath}`);
  }
  return resolved;
}

async function readCacheFile(loaded: LoadedProjectConfig, relativePath: string): Promise<string> {
  const filePath = cachePath(loaded, relativePath);
  if (!existsSync(filePath)) {
    throw new Error(`Project Memory cache file not found: ${path.relative(loaded.projectRoot, filePath).replace(/\\/g, "/")}. Run memory build first.`);
  }
  return readFile(filePath, "utf8");
}

async function cacheResource(uri: URL, relativePath: string) {
  const loaded = await loadMemoryConfig();
  const text = await readCacheFile(loaded, relativePath);
  return {
    contents: [
      {
        uri: uri.toString(),
        mimeType: mimeTypeFor(relativePath),
        text
      }
    ]
  };
}

function safeCacheStem(value: string, label: string): string {
  const decoded = decodeURIComponent(value).trim().replace(/\.md$/i, "");
  if (!decoded || decoded.includes("..") || /[\\/]/.test(decoded) || !/^[A-Za-z0-9._-]+$/.test(decoded)) {
    throw new Error(`${label} must be a cache file stem using letters, numbers, dot, underscore, or dash.`);
  }
  return decoded;
}

async function listFilesIfPresent(dir: string, extension?: string): Promise<string[]> {
  if (!existsSync(dir)) return [];
  const entries = await readdir(dir, { withFileTypes: true });
  return entries
    .filter((entry) => entry.isFile())
    .filter((entry) => !extension || entry.name.toLowerCase().endsWith(extension))
    .map((entry) => entry.name)
    .sort((a, b) => a.localeCompare(b));
}

async function readPatternsJsonl(loaded: LoadedProjectConfig): Promise<Array<Record<string, unknown>>> {
  const text = await readCacheFile(loaded, path.join("patterns", "patterns.jsonl"));
  return text
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(Boolean)
    .map((line) => JSON.parse(line) as Record<string, unknown>);
}

async function validationAggregateResource(uri: URL) {
  const loaded = await loadMemoryConfig();
  const validationDir = cachePath(loaded, "validation");
  if (!existsSync(validationDir)) {
    throw new Error("Project Memory validation cache not found. Run memory build --capsule first.");
  }
  const names = await listFilesIfPresent(validationDir, ".json");
  const files = [];
  for (const name of names) {
    const text = await readCacheFile(loaded, path.join("validation", name));
    files.push({
      name,
      content: JSON.parse(text)
    });
  }

  return {
    contents: [
      {
        uri: uri.toString(),
        mimeType: "application/json",
        text: `${JSON.stringify({ files }, null, 2)}\n`
      }
    ]
  };
}

async function patternResource(uri: URL, name: string) {
  const loaded = await loadMemoryConfig();
  const lookup = decodeURIComponent(name).trim().toLowerCase();
  const patterns = await readPatternsJsonl(loaded);
  const pattern = patterns.find((item) => {
    const id = typeof item.id === "string" ? item.id.toLowerCase() : "";
    const title = typeof item.title === "string" ? item.title.toLowerCase() : "";
    return id === lookup || title === lookup;
  });
  if (!pattern) {
    throw new Error(`Project Memory pattern cache entry not found: ${name}`);
  }

  return {
    contents: [
      {
        uri: uri.toString(),
        mimeType: "application/json",
        text: `${JSON.stringify(pattern, null, 2)}\n`
      }
    ]
  };
}

async function systemResource(uri: URL, name: string) {
  const systemId = safeCacheStem(name, "system name");
  return cacheResource(uri, path.join("summaries", "systems", `${systemId}.md`));
}

async function summaryResource(uri: URL, kind: string, id: string) {
  const summaryKind = safeCacheStem(kind, "summary kind");
  const summaryId = safeCacheStem(id, "summary id");
  return cacheResource(uri, path.join("summaries", summaryKind, `${summaryId}.md`));
}

async function listSystemResources() {
  const loaded = await loadMemoryConfig();
  const summariesDir = cachePath(loaded, path.join("summaries", "systems"));
  const names = await listFilesIfPresent(summariesDir, ".md");
  return {
    resources: names.map((name) => {
      const systemId = path.basename(name, ".md");
      return {
        uri: `project://system/${encodeURIComponent(systemId)}`,
        name: systemId,
        title: `System ${systemId}`,
        description: "Project Memory cached system summary.",
        mimeType: "text/markdown"
      };
    })
  };
}

async function listPatternResources() {
  const loaded = await loadMemoryConfig();
  if (!existsSync(cachePath(loaded, path.join("patterns", "patterns.jsonl")))) {
    return { resources: [] };
  }
  const patterns = await readPatternsJsonl(loaded);
  return {
    resources: patterns.map((pattern) => {
      const id = String(pattern.id ?? pattern.title ?? "pattern");
      return {
        uri: `project://pattern/${encodeURIComponent(id)}`,
        name: id,
        title: String(pattern.title ?? id),
        description: "Project Memory cached pattern entry.",
        mimeType: "application/json"
      };
    })
  };
}

async function listSummaryResources() {
  const loaded = await loadMemoryConfig();
  const summariesRoot = cachePath(loaded, "summaries");
  if (!existsSync(summariesRoot)) return { resources: [] };
  const kinds = (await readdir(summariesRoot, { withFileTypes: true }))
    .filter((entry) => entry.isDirectory())
    .map((entry) => entry.name)
    .sort((a, b) => a.localeCompare(b));
  const resources = [];
  for (const kind of kinds) {
    const dir = path.join(summariesRoot, kind);
    const names = await listFilesIfPresent(dir, ".md");
    for (const name of names) {
      const id = path.basename(name, ".md");
      resources.push({
        uri: `project://summary/${encodeURIComponent(kind)}/${encodeURIComponent(id)}`,
        name: `${kind}/${id}`,
        title: `${kind}/${id}`,
        description: "Project Memory cached summary.",
        mimeType: "text/markdown"
      });
    }
  }
  return { resources };
}

async function queryMemoryCache(query: string, types: string[] | undefined, limit: number): Promise<MemorySearchResult[]> {
  const loaded = await loadMemoryConfig();
  const ftsPath = cachePath(loaded, path.join("index", "fts.sqlite"));
  if (!existsSync(ftsPath)) {
    throw new Error(`Project Memory full-text index not found at ${path.relative(loaded.projectRoot, ftsPath).replace(/\\/g, "/")}. Run memory build --docs-only.`);
  }
  const requestedTypes = new Set((types ?? []).map((type) => type.toLowerCase()));
  const index = FullTextIndex.open(ftsPath);
  try {
    return index
      .search(query, Math.max(limit * 3, limit))
      .filter((result) => requestedTypes.size === 0 || requestedTypes.has(result.sourceType.toLowerCase()))
      .slice(0, limit);
  } finally {
    index.close();
  }
}

function normalizeContextTaskType(value: string | undefined): ContextTaskType | undefined {
  if (!value) return undefined;
  if ((contextTaskTypes as readonly string[]).includes(value)) return value as ContextTaskType;
  throw new Error(`Unsupported taskType: ${value}. Expected one of: ${contextTaskTypes.join(", ")}.`);
}

async function owningSystemFor(query: string) {
  const loaded = await loadMemoryConfig();
  const graphClient = createProjectGraphClient(loaded);
  const pack = await buildMemoryContextPack({ query }, { loaded, graphClient });
  return {
    query,
    owningSystem: pack.owningSystem,
    secondarySystems: pack.secondarySystems,
    graphStatus: pack.graphContext.status,
    missingInformation: pack.missingInformation
  };
}

async function patternsFor(query: string, limit: number): Promise<EvidenceItem[]> {
  const loaded = await loadMemoryConfig();
  return findExistingPatterns(query, loaded, limit);
}

async function impactFor(queryOrNodeIds: string | string[], depth: number) {
  const loaded = await loadMemoryConfig();
  const graphClient = createProjectGraphClient(loaded);
  const graphStatus = await graphClient.status();
  const query = typeof queryOrNodeIds === "string" ? queryOrNodeIds : undefined;
  const feature = query ? await graphClient.findFeatureContext(query, { limit: 12 }) : undefined;
  const nodeIds = Array.isArray(queryOrNodeIds)
    ? queryOrNodeIds
    : feature?.nodes.map((node) => node.id) ?? [];
  const uniqueNodeIds = [...new Set(nodeIds.filter(Boolean))];
  const [impact, expandedContext] = await Promise.all([
    graphClient.impactAnalysis(uniqueNodeIds, { limit: Math.max(40, depth * 80) }),
    graphClient.expandContext(uniqueNodeIds, { depth, limit: Math.max(40, depth * 80) })
  ]);
  return {
    query,
    requestedDepth: depth,
    graphStatus,
    resolvedNodeIds: uniqueNodeIds,
    feature,
    impact,
    expandedContext,
    warnings: [
      ...graphStatus.warnings,
      ...(feature?.warnings ?? []),
      ...impact.warnings,
      ...expandedContext.warnings
    ]
  };
}

function workflowTemplate(kind: "feature" | "debug" | "refactor" | "blueprint"): string {
  const action =
    kind === "feature" ? "实现 UE 功能" :
    kind === "debug" ? "排查并修复问题" :
    kind === "refactor" ? "执行重构" :
    "修改 Blueprint / Material / 资产相关逻辑";
  const extra = kind === "blueprint"
    ? [
        "6. 对蓝图/材质/资产先 read/analyze，再 dry-run，再 apply；默认不要保存资产。",
        "7. 检查蓝图编译或 UE MCP 返回的 warnings/errors。"
      ]
    : [
        "6. 做最小必要修改，并复用 context pack 中的现有 owner 和模式。",
        "7. 按 validationPlan 执行最快可用验证。"
      ];

  return [
    `目标：${action}，但必须先使用 Project Memory。`,
    "",
    "固定流程：",
    "1. 调用 memory_status。",
    "2. 如果状态不是 fresh，先调用 update_project_memory；如果提示 needs_full_rebuild 或缓存缺失，则调用 build_project_memory。",
    "3. 调用 get_context_pack，query 使用当前任务描述。",
    "4. 修改前先向用户说明 owning system、现有模式、计划复用的变量/类/蓝图/资产、验证计划。",
    "5. 不要直接编辑 .uasset、.umap 或其他 UE 二进制资源。",
    ...extra,
    "8. 最终回复包含实现思路、修改文件、验证命令和结果、未验证风险。"
  ].join("\n");
}

export function createProjectMemoryMcpServer(): McpServer {
  const server = new McpServer(
    {
      name: "project-memory-system",
      version: "0.1.0",
      description: "Portable Project Memory System MCP server."
    },
    {
      instructions: SERVER_INSTRUCTIONS
    }
  );

  server.registerTool(
    "memory_status",
    {
      title: "Memory Status",
      description: "Return Project Memory cache freshness and source status.",
      inputSchema: z.object({})
    },
    async () => textResult(await memoryStatus(projectOptions()))
  );

  server.registerTool(
    "build_project_memory",
    {
      title: "Build Project Memory",
      description: "Build Project Memory caches. Writes only memoryOutputRoot; does not edit Unreal assets.",
      inputSchema: z.object({
        mode: z.enum(["full", "incremental"]),
        docsOnly: z.boolean().optional(),
        includeGraph: z.boolean().optional()
      })
    },
    async ({ mode, docsOnly, includeGraph }) => {
      const result = mode === "incremental"
        ? await updateMemory(projectOptions())
        : await buildMemory({ ...projectOptions(), docsOnly: docsOnly ?? false });
      return textResult({
        ...result,
        requestedMode: mode,
        includeGraph: includeGraph ?? true,
        graphNote: "Project Memory reads graph context through adapters; this tool does not build or modify ProjectGraph."
      });
    }
  );

  server.registerTool(
    "update_project_memory",
    {
      title: "Update Project Memory",
      description: "Incrementally update Project Memory document caches. Writes only memoryOutputRoot.",
      inputSchema: z.object({
        changedPaths: z.array(z.string()).optional(),
        includeGraph: z.boolean().optional()
      })
    },
    async ({ changedPaths, includeGraph }) => textResult({
      ...(await updateMemory(projectOptions())),
      changedPaths: changedPaths ?? [],
      includeGraph: includeGraph ?? true,
      graphNote: "Project Memory update records graph cache signatures when configured, but does not rebuild ProjectGraph."
    })
  );

  server.registerTool(
    "query_project_memory",
    {
      title: "Query Project Memory",
      description: "Query the Project Memory full-text index.",
      inputSchema: z.object({
        query: z.string().min(1),
        types: z.array(z.string()).optional(),
        limit: z.number().int().min(1).max(100).optional()
      })
    },
    async ({ query, types, limit }) => textResult({
      query,
      types: types ?? [],
      limit: limit ?? 10,
      results: await queryMemoryCache(query, types, limit ?? 10)
    })
  );

  server.registerTool(
    "get_project_capsule",
    {
      title: "Get Project Capsule",
      description: "Read .ai/project-memory/project_capsule.md.",
      inputSchema: z.object({})
    },
    async () => {
      const loaded = await loadMemoryConfig();
      return textResult({
        uri: "project://capsule",
        text: await readCacheFile(loaded, "project_capsule.md")
      });
    }
  );

  server.registerTool(
    "find_owning_system",
    {
      title: "Find Owning System",
      description: "Find the most likely owning system for a task using Project Memory and the graph adapter.",
      inputSchema: z.object({
        query: z.string().min(1)
      })
    },
    async ({ query }) => textResult(await owningSystemFor(query))
  );

  server.registerTool(
    "find_existing_patterns",
    {
      title: "Find Existing Patterns",
      description: "Find cached implementation patterns relevant to a query.",
      inputSchema: z.object({
        query: z.string().min(1),
        limit: z.number().int().min(1).max(100).optional()
      })
    },
    async ({ query, limit }) => textResult({
      query,
      patterns: await patternsFor(query, limit ?? 10)
    })
  );

  server.registerTool(
    "get_context_pack",
    {
      title: "Get Context Pack",
      description: "Build a Project Memory context pack for implementation, debugging, refactor, blueprint, material, asset, or question tasks.",
      inputSchema: z.object({
        query: z.string().min(1),
        taskType: z.string().optional(),
        budget: z.object({
          maxDocs: z.number().int().min(0).optional(),
          maxSystems: z.number().int().min(0).optional(),
          maxPatterns: z.number().int().min(0).optional(),
          maxGraphNodes: z.number().int().min(0).optional(),
          maxFiles: z.number().int().min(0).optional(),
          maxBlueprints: z.number().int().min(0).optional(),
          maxAssets: z.number().int().min(0).optional()
        }).optional()
      })
    },
    async ({ query, taskType, budget }) => textResult(await getContextPack({
      query,
      taskType: normalizeContextTaskType(taskType),
      budget
    }, projectOptions()))
  );

  server.registerTool(
    "impact_analysis",
    {
      title: "Impact Analysis",
      description: "Analyze graph impact for node ids or a query through the ProjectGraphClient adapter.",
      inputSchema: z.object({
        queryOrNodeIds: z.union([z.string().min(1), z.array(z.string()).min(1)]),
        depth: z.number().int().min(1).max(5).optional()
      })
    },
    async ({ queryOrNodeIds, depth }) => textResult(await impactFor(queryOrNodeIds, depth ?? 1))
  );

  server.registerResource(
    "project_capsule",
    "project://capsule",
    {
      title: "Project Capsule",
      description: "Cached project capsule from .ai/project-memory/project_capsule.md.",
      mimeType: "text/markdown"
    },
    async (uri) => cacheResource(uri, "project_capsule.md")
  );

  server.registerResource(
    "project_architecture_rules",
    "project://architecture-rules",
    {
      title: "Architecture Rules",
      description: "Cached architecture rules from .ai/project-memory/architecture_rules.md.",
      mimeType: "text/markdown"
    },
    async (uri) => cacheResource(uri, "architecture_rules.md")
  );

  server.registerResource(
    "project_system_map",
    "project://system-map",
    {
      title: "System Map",
      description: "Cached system map from .ai/project-memory/system_map.json.",
      mimeType: "application/json"
    },
    async (uri) => cacheResource(uri, "system_map.json")
  );

  server.registerResource(
    "project_patterns",
    "project://patterns",
    {
      title: "Patterns",
      description: "Cached pattern library from .ai/project-memory/patterns/patterns.jsonl.",
      mimeType: "application/jsonl"
    },
    async (uri) => cacheResource(uri, path.join("patterns", "patterns.jsonl"))
  );

  server.registerResource(
    "project_validation",
    "project://validation",
    {
      title: "Validation Memory",
      description: "Aggregated validation cache from .ai/project-memory/validation/*.json.",
      mimeType: "application/json"
    },
    async (uri) => validationAggregateResource(uri)
  );

  server.registerResource(
    "project_ue_cache",
    "project://ue-cache",
    {
      title: "UE Cache Summaries",
      description: "Read-only Blueprint, asset registry, and material summaries generated from .ai/cache into .ai/project-memory/ue-cache/summaries.jsonl.",
      mimeType: "application/jsonl"
    },
    async (uri) => cacheResource(uri, path.join("ue-cache", "summaries.jsonl"))
  );

  server.registerResource(
    "project_system",
    new ResourceTemplate("project://system/{name}", { list: listSystemResources }),
    {
      title: "System Summary",
      description: "Cached system summary from .ai/project-memory/summaries/systems/{name}.md.",
      mimeType: "text/markdown"
    },
    async (uri, variables) => systemResource(uri, String(variables.name))
  );

  server.registerResource(
    "project_pattern",
    new ResourceTemplate("project://pattern/{name}", { list: listPatternResources }),
    {
      title: "Pattern Entry",
      description: "Cached pattern entry selected from .ai/project-memory/patterns/patterns.jsonl.",
      mimeType: "application/json"
    },
    async (uri, variables) => patternResource(uri, String(variables.name))
  );

  server.registerResource(
    "project_summary",
    new ResourceTemplate("project://summary/{kind}/{id}", { list: listSummaryResources }),
    {
      title: "Summary",
      description: "Cached summary from .ai/project-memory/summaries/{kind}/{id}.md.",
      mimeType: "text/markdown"
    },
    async (uri, variables) => summaryResource(uri, String(variables.kind), String(variables.id))
  );

  server.registerPrompt(
    "implement_feature_with_project_memory",
    {
      title: "Implement Feature With Project Memory",
      description: "固定工作流：先检查 Project Memory，再拿 context pack，然后实现 UE 功能。"
    },
    async () => promptResult("Implement a UE feature with Project Memory gates.", workflowTemplate("feature"))
  );

  server.registerPrompt(
    "debug_with_project_memory",
    {
      title: "Debug With Project Memory",
      description: "固定工作流：用 Project Memory 定位 owner、模式和验证计划后再修复。"
    },
    async () => promptResult("Debug with Project Memory gates.", workflowTemplate("debug"))
  );

  server.registerPrompt(
    "refactor_with_project_memory",
    {
      title: "Refactor With Project Memory",
      description: "固定工作流：先确认 owning system 和现有边界，再做最小重构。"
    },
    async () => promptResult("Refactor with Project Memory gates.", workflowTemplate("refactor"))
  );

  server.registerPrompt(
    "modify_blueprint_with_project_memory",
    {
      title: "Modify Blueprint With Project Memory",
      description: "固定工作流：蓝图/材质/资产修改前读取记忆和图谱，不直接编辑 .uasset。"
    },
    async () => promptResult("Modify Blueprint with Project Memory gates.", workflowTemplate("blueprint"))
  );

  return server;
}

async function main(): Promise<void> {
  const server = createProjectMemoryMcpServer();
  const transport = new StdioServerTransport();
  await server.connect(transport);
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().catch((error: unknown) => {
    console.error(error instanceof Error ? error.message : String(error));
    process.exit(1);
  });
}
