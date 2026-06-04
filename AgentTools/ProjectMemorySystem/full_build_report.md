# Project Memory Full Build Report

本报告说明 full memory build 预期生成的缓存形态。它是 Project Memory System 的工具文档，不代表当前 UE gameplay 已实现事实。

## Full Build Outputs

- `.ai/project-memory/index/documents.jsonl`
- `.ai/project-memory/index/fts.sqlite`
- `.ai/project-memory/project_capsule.md`
- `.ai/project-memory/architecture_rules.md`
- `.ai/project-memory/system_map.json`
- `.ai/project-memory/summaries/systems/*.md`
- `.ai/project-memory/patterns/patterns.jsonl`
- `.ai/project-memory/validation/*.json`
- `.ai/project-memory/ue-cache/summaries.jsonl`
- `.ai/project-memory/runs/<runId>/implementation_map.json`
- `.ai/project-memory/runs/latest.json`

## Implementation Memory

`implementation_map.json` 缓存已实现功能和现有实现模式：

- `features`: 已有功能记录，包括用途、所属系统、关键代码、蓝图、资产、符号、变量、事件、DataAsset、扩展建议、禁止做法和验证提示。
- `patterns`: 已有实现模式记录，包括推荐做法、禁止做法、可复用符号、蓝图和资产。
- `keySymbols`: 从 ProjectKnowledge、ProjectGraph adapter 和 UE cache summaries 提取的可复用符号。
- `reuseGuidance`: 有证据的复用和扩展建议。
- `graphLinks`: feature 到 ProjectGraph node id 的链接。
- `blueprintLinks`: feature 到蓝图摘要的链接。
- `assetLinks`: feature 到资产/材质摘要的链接。
- `missingInformation`: 证据不足或缓存缺失的事项。

所有记录必须带 `sourcePaths` 和 `sourceHash`。如果没有证据，写入 `TODO / missing evidence` 或 `missingInformation`，不要编造项目事实。

## Context Pack Merge

`memory context-pack <query>` 会合并 Implementation Memory 并返回：

- `relevantImplementations`
- `existingPatterns`
- `reusableSymbols`
- `reusableBlueprints`
- `reusableAssets`
- `forbiddenApproaches`
- `extensionGuidance`

实现 UE 功能前，Codex 应先说明 owning system、现有实现模式、可复用变量/类/组件/蓝图/DataAsset、禁止做法和验证计划。

## Validation

```powershell
cd AgentTools/ProjectMemorySystem
npm test
node dist/src/cli.js memory build --project ../..
node dist/src/cli.js memory context-pack "enemy health bar damage UI" --project ../..
```

验收重点：

- full build 生成 `.ai/project-memory/runs/<runId>/implementation_map.json`。
- `runs/latest.json` 指向最近一次 implementation map。
- Context Pack 能返回相关 implementation、pattern、可复用符号、蓝图/资产和 forbidden approaches。
- 工具不直接编辑 `.uasset`、`.umap` 或 gameplay 代码。
