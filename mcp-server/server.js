import { McpServer } from "@modelcontextprotocol/server";
import { StdioServerTransport } from "@modelcontextprotocol/server/stdio";
import * as z from "zod/v4";
import yaml from "js-yaml";

const BRIDGE_URL = process.env.ABT_BRIDGE_URL || "http://127.0.0.1:31055";
const BRIDGE_TOKEN = process.env.ABT_BRIDGE_TOKEN || "change-me-local";

async function callBridge(path, payload = undefined, method = "POST") {
  const headers = { accept: "application/json" };
  if (BRIDGE_TOKEN) headers["x-abt-token"] = BRIDGE_TOKEN;
  if (payload !== undefined) headers["content-type"] = "application/json";

  const res = await fetch(`${BRIDGE_URL}${path}`, {
    method,
    headers,
    body: payload !== undefined ? JSON.stringify(payload) : undefined
  });

  const text = await res.text();
  let json = {};
  try {
    json = text ? JSON.parse(text) : {};
  } catch (err) {
    throw new Error(`UE bridge returned non-JSON response from ${path}: ${text.slice(0, 500)}`);
  }

  if (!res.ok || json.ok === false) {
    const message = json.error || json.message || `${path} failed with HTTP ${res.status}`;
    throw new Error(message);
  }

  return json;
}

function normalizePatch(input) {
  if (typeof input === "string") {
    const parsed = yaml.load(input);
    if (!parsed || typeof parsed !== "object") {
      throw new Error("Patch YAML did not parse to an object.");
    }
    return parsed;
  }
  return input;
}

function textResult(value) {
  return {
    content: [{ type: "text", text: typeof value === "string" ? value : JSON.stringify(value, null, 2) }]
  };
}

const server = new McpServer({
  name: "ue5-agent-blueprint-tools",
  version: "0.1.0"
});

server.registerTool(
  "ping_ue_bridge",
  {
    description: "Check whether the UE5 AgentBlueprintTools localhost bridge is alive.",
    inputSchema: z.object({})
  },
  async () => textResult(await callBridge("/v1/health", undefined, "GET"))
);

server.registerTool(
  "read_blueprint",
  {
    description: "Export a Blueprint asset as JSON IR: variables, components, graphs, nodes, pins, links, and semantic summary.",
    inputSchema: z.object({ assetPath: z.string().describe("UE asset path, e.g. /Game/Blueprints/BP_Door") })
  },
  async ({ assetPath }) => textResult(await callBridge("/v1/blueprint/read", { assetPath }))
);

server.registerTool(
  "analyze_blueprint_graph",
  {
    description: "Analyze a Blueprint graph and return entry points, execution paths, variable reads/writes, and external calls.",
    inputSchema: z.object({
      assetPath: z.string(),
      graphName: z.string().optional().default("")
    })
  },
  async ({ assetPath, graphName }) => textResult(await callBridge("/v1/blueprint/analyze", { assetPath, graphName }))
);

server.registerTool(
  "list_callable_functions",
  {
    description: "List reflected BlueprintCallable functions that match a query.",
    inputSchema: z.object({
      query: z.string().optional().default(""),
      limit: z.number().int().min(1).max(200).optional().default(50)
    })
  },
  async ({ query, limit }) => textResult(await callBridge("/v1/blueprint/functions", { query, limit }))
);

server.registerTool(
  "dry_run_blueprint_patch",
  {
    description: "Validate a Blueprint Patch DSL document without modifying assets.",
    inputSchema: z.object({ patch: z.union([z.string(), z.record(z.any())]) })
  },
  async ({ patch }) => textResult(await callBridge("/v1/blueprint/patch/dry-run", { patch: normalizePatch(patch) }))
);

server.registerTool(
  "apply_blueprint_patch",
  {
    description: "Apply a Blueprint Patch DSL document. The UE plugin compiles and rolls back on failure.",
    inputSchema: z.object({
      patch: z.union([z.string(), z.record(z.any())]),
      saveOnSuccess: z.boolean().optional().default(false)
    })
  },
  async ({ patch, saveOnSuccess }) => textResult(await callBridge("/v1/blueprint/patch/apply", {
    patch: normalizePatch(patch),
    saveOnSuccess
  }))
);

server.registerTool(
  "compile_blueprint",
  {
    description: "Compile a Blueprint and return status plus compiler log summary.",
    inputSchema: z.object({ assetPath: z.string() })
  },
  async ({ assetPath }) => textResult(await callBridge("/v1/blueprint/compile", { assetPath }))
);

server.registerTool(
  "read_material",
  {
    description: "Read a material or material instance summary.",
    inputSchema: z.object({ assetPath: z.string() })
  },
  async ({ assetPath }) => textResult(await callBridge("/v1/material/read", { assetPath }))
);

server.registerTool(
  "patch_material",
  {
    description: "Apply a Material Patch DSL document for expressions, material properties, and material instance parameters.",
    inputSchema: z.object({
      patch: z.union([z.string(), z.record(z.any())]),
      saveOnSuccess: z.boolean().optional().default(false)
    })
  },
  async ({ patch, saveOnSuccess }) => textResult(await callBridge("/v1/material/patch", {
    patch: normalizePatch(patch),
    saveOnSuccess
  }))
);

server.registerTool(
  "create_asset",
  {
    description: "Create a supported asset: Blueprint, Material, or MaterialInstanceConstant.",
    inputSchema: z.object({
      assetType: z.enum(["Blueprint", "Material", "MaterialInstanceConstant", "UserDefinedStruct"]),
      path: z.string(),
      parentClass: z.string().optional(),
      parentMaterial: z.string().optional(),
      fields: z.array(z.object({
        name: z.string(),
        type: z.string().optional(),
        default: z.string().optional()
      })).optional(),
      save: z.boolean().optional().default(false)
    })
  },
  async (args) => textResult(await callBridge("/v1/asset/create", args))
);

server.registerTool(
  "read_asset",
  {
    description: "Read an asset summary and optional simple reflected properties. Use object=DefaultObject for Blueprint CDO values.",
    inputSchema: z.object({
      assetPath: z.string(),
      object: z.enum(["Asset", "DefaultObject"]).optional().default("Asset"),
      properties: z.array(z.string()).optional().default([])
    })
  },
  async (args) => textResult(await callBridge("/v1/asset/read", args))
);

server.registerTool(
  "set_asset_property",
  {
    description: "Set an editor property on an asset or Blueprint CDO using reflection.",
    inputSchema: z.object({
      assetPath: z.string(),
      object: z.enum(["Asset", "DefaultObject"]).optional().default("Asset"),
      propertyPath: z.string(),
      value: z.any(),
      save: z.boolean().optional().default(false)
    })
  },
  async (args) => textResult(await callBridge("/v1/asset/set-property", args))
);

server.registerTool(
  "save_asset",
  {
    description: "Save a loaded asset by UE asset path.",
    inputSchema: z.object({ assetPath: z.string() })
  },
  async ({ assetPath }) => textResult(await callBridge("/v1/asset/save", { assetPath }))
);

server.registerTool(
  "delete_asset",
  {
    description: "Delete an asset through the UE Editor asset API. Use only for known temporary or explicitly requested assets.",
    inputSchema: z.object({ assetPath: z.string() })
  },
  async ({ assetPath }) => textResult(await callBridge("/v1/asset/delete", { assetPath }))
);

server.registerTool(
  "place_actor",
  {
    description: "Place an Actor Blueprint or Actor class into the current editor level.",
    inputSchema: z.object({
      assetPath: z.string(),
      label: z.string().optional(),
      location: z.array(z.number()).length(3).optional(),
      rotation: z.array(z.number()).length(3).optional(),
      transient: z.boolean().optional().default(false),
      saveLevel: z.boolean().optional().default(false)
    })
  },
  async (args) => textResult(await callBridge("/v1/level/place-actor", args))
);

async function main() {
  const transport = new StdioServerTransport();
  await server.connect(transport);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
