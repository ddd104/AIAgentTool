import test from "node:test";
import assert from "node:assert/strict";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { GraphStore } from "../src/graph/graphStore.js";
import { indexUeEditorCache } from "../src/indexers/ueEditorIndexer.js";

const __dirname = path.dirname(fileURLToPath(import.meta.url));

test("ueEditorIndexer builds parent class and asset reference edges from fixtures", async () => {
  const store = new GraphStore();
  const cacheRoot = path.resolve(__dirname, "../../test/fixtures/ue-cache");

  const result = await indexUeEditorCache({ projectRoot: cacheRoot, cacheRoot }, store);

  assert.equal(result.blueprintFiles, 1);
  assert.equal(result.assetRegistryFiles, 1);
  assert.equal(result.materialFiles, 1);

  const edges = store.edges();
  assert.ok(edges.some((edge) => edge.type === "PARENT_CLASS"));
  assert.ok(edges.some((edge) => edge.type === "REFERENCES_ASSET"));
  assert.ok(edges.some((edge) => edge.type === "HAS_VARIABLE"));
  assert.ok(edges.some((edge) => edge.type === "HAS_FUNCTION"));
  assert.ok(edges.some((edge) => edge.type === "HAS_MACRO"));
  assert.ok(edges.some((edge) => edge.type === "HAS_COMPONENT"));
  assert.ok(edges.some((edge) => edge.type === "DEPENDS_ON"));

  const nodes = store.nodes();
  assert.ok(nodes.some((node) => node.type === "Blueprint" && node.name === "BP_TestWidget"));
  assert.ok(nodes.some((node) => node.type === "Macro" && node.name === "FormatTitle"));
  assert.ok(nodes.some((node) => node.type === "Widget"));
  assert.ok(nodes.some((node) => node.type === "Material"));

  const refresh = nodes.find((node) => node.type === "Function" && node.name === "Refresh");
  assert.ok(refresh);
  assert.equal((refresh.metadata.raw as { nodes?: unknown[] }).nodes?.length, 1);

  const rootCanvas = nodes.find((node) => node.type === "Component" && node.name === "RootCanvas");
  assert.ok(rootCanvas);
  assert.equal(
    ((rootCanvas.metadata.raw as { defaultProperties?: Record<string, string> }).defaultProperties ?? {}).Visibility,
    "Visible"
  );
});
