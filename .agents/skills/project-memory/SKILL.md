---
name: project-memory
description: 当任务涉及功能实现、bug 修复、重构、UE C++、蓝图、资产、材质、系统设计、项目架构理解时触发。
allow_implicit_invocation: true
---

# Project Memory Skill

本 skill 是本项目 Codex 获取上下文的主要入口。Project Memory 负责整合项目资料、文档、缓存摘要、patterns 和 ProjectGraph adapter 结果；ProjectGraph 只作为底层图谱工具。

## 固定工作流

1. 调用 `memory_status`，确认 Project Memory 缓存状态。
2. 如果状态是 `missing`、`stale_config`、`stale_sources`、`stale_graph` 或 `needs_full_rebuild`，先调用 `update_project_memory`；如果工具提示必须完整重建，再调用 `build_project_memory`。
3. 调用 `get_context_pack`，query 使用当前任务的真实目标，taskType 按功能、修复、重构、蓝图、材质、资产或问题选择。
4. 阅读 context pack 返回的文件、蓝图、文档、patterns、architecture rules、validation plan 和 missingInformation。
5. 修改前必须说明：owning system、现有框架、计划复用的变量/类/组件/蓝图/DataAsset/patterns、验证计划。
6. 只做任务必需的最小实现，优先复用已有内容，不随便新建 Manager 或 Subsystem。
7. 修改后调用 `impact_analysis`，用变更文件、节点或任务 query 分析影响范围。
8. 修改后调用 `update_project_memory`，传入 changedPaths，更新 Project Memory 中的变更资料或缓存签名。

## 禁止事项

- 不允许未查询 Project Memory 就开始功能实现、bug 修复、重构、系统设计或架构判断。
- 不允许直接编辑 `.uasset`、`.umap` 或其他 UE 二进制资源。
- 不允许凭空猜测蓝图、材质、资产、DataAsset 或配置内容。
- 不允许在没有 context pack 证据时随便新建 Manager、Subsystem、全局单例或新的框架层。

## ProjectGraph 使用边界

- 优先通过 `project_memory` 的 `get_context_pack` 获取上下文。
- 只有在需要底层图谱细节、Project Memory 明确缺少图谱证据、或需要重建/更新图谱时，才直接调用 `ue_project_graph`。
- 直接调用 `ue_project_graph` 后，也要回到 Project Memory 的 context pack 汇总结果，避免只依据局部图谱节点做实现。
