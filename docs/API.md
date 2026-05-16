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

## POST /v1/asset/create

```json
{ "assetType": "Material", "path": "/Game/Materials/M_New", "save": true }
```
