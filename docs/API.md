# Bridge API

All routes are local HTTP routes exposed by the UE Editor plugin.

Header:

```text
x-abt-token: change-me-local
```

## GET /v1/health

Returns bridge status.

## POST /v1/blueprint/read

```json
{ "assetPath": "/Game/Blueprints/BP_Door" }
```

## POST /v1/blueprint/analyze

```json
{ "assetPath": "/Game/Blueprints/BP_Door", "graphName": "Interact" }
```

## POST /v1/blueprint/functions

```json
{ "query": "print string", "limit": 50 }
```

## POST /v1/blueprint/patch/dry-run

```json
{ "patch": { "target": "/Game/Blueprints/BP_Door", "operations": [] } }
```

## POST /v1/blueprint/patch/apply

```json
{ "patch": { "target": "/Game/Blueprints/BP_Door", "operations": [] }, "saveOnSuccess": false }
```

## POST /v1/blueprint/compile

```json
{ "assetPath": "/Game/Blueprints/BP_Door" }
```

## POST /v1/material/read

```json
{ "assetPath": "/Game/Materials/M_Door" }
```

## POST /v1/material/patch

```json
{ "patch": { "target": "/Game/Materials/M_Door", "operations": [] }, "saveOnSuccess": false }
```

## POST /v1/performance/analyze

Scans textures, materials, material instances, static meshes, skeletal meshes, and project settings for target-platform performance risks.

```json
{
  "targetPlatforms": ["Android", "Windows", "Linux"],
  "profile": "balanced",
  "contentPaths": ["/Game"],
  "maxAssets": 2000
}
```

## POST /v1/performance/optimization/dry-run

Builds a no-write optimization plan. The plan can include texture settings, material flags, and `Config/DefaultEngine.ini` project settings.

```json
{
  "targetPlatforms": ["Android"],
  "profile": "mobile",
  "allowVisualChanges": false
}
```

## POST /v1/performance/optimization/apply

Applies generated safe actions. Asset saving and visual-risk material changes must be requested explicitly.

```json
{
  "targetPlatforms": ["Android", "Windows", "Linux"],
  "profile": "balanced",
  "saveAssets": false,
  "writeConfig": true,
  "applyVisualChanges": false,
  "maxActions": 200
}
```

## POST /v1/asset/create

```json
{ "assetType": "Material", "path": "/Game/Materials/M_New", "save": true }
```
