import type { GraphFeatureResult } from "../graph/projectGraphClient.js";
import type { EvidenceItem, OwningSystemContext } from "../types/contextPackTypes.js";

function tokenScore(query: string, text: string): number {
  const tokens = query.toLowerCase().split(/\s+/).filter(Boolean);
  const haystack = text.toLowerCase();
  return tokens.reduce((score, token) => score + (haystack.includes(token) ? 1 : 0), 0);
}

export function findOwningSystem(query: string, systems: EvidenceItem[], graphFeature: GraphFeatureResult): OwningSystemContext {
  if (graphFeature.owningSystem) {
    return {
      id: graphFeature.owningSystem,
      title: graphFeature.owningSystem,
      confidence: "high",
      sourcePaths: graphFeature.filesToRead,
      evidence: graphFeature.nodes.slice(0, 3).map((node) => node.summary ?? node.name)
    };
  }

  const scoredSystems = systems
    .map((system) => ({
      system,
      score: (system.score ?? 0) + tokenScore(query, `${system.title} ${system.excerpt ?? ""}`)
    }))
    .sort((a, b) => b.score - a.score);
  const best = scoredSystems[0];
  if (!best || best.score <= 0) {
    return {
      confidence: "none",
      sourcePaths: [],
      evidence: []
    };
  }

  return {
    id: best.system.id,
    title: best.system.title,
    confidence: best.score >= 3 ? "medium" : "low",
    sourcePaths: best.system.sourcePaths,
    sourceHash: best.system.sourceHash,
    evidence: best.system.excerpt ? [best.system.excerpt] : []
  };
}
