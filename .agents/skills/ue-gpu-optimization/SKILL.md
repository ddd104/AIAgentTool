---
name: ue-gpu-optimization
description: 当针对 GPU 帧时间、渲染性能、材质、阴影、Lumen、Nanite、VSM、后处理、半透明效果、Niagara、SceneCapture 或显存（VRAM）对虚幻引擎（Unreal Engine）项目进行优化时，请使用此技能。
---

# UE GPU 优化工作流

请严格遵循此工作流。

## 1. 基线确立

首先收集基线数据：

1. 运行 `stat unit` 命令。
2. 判定项目当前是受 GPU 瓶颈限制、绘制线程（Draw-thread）瓶颈限制、游戏线程（Game-thread）瓶颈限制，还是处于混合瓶颈状态。
3. 若判定为受 GPU 瓶颈限制，请运行 `stat gpu` 和 `ProfileGPU` 命令。
4. 记录当前地图名称、摄像机位置、分辨率、可伸缩性设置（Scalability）、目标平台、RHI 接口类型以及构建配置（Build Config）。
5. 将基线数据保存为文件：`PerfReports/baseline_<timestamp>.json`。

在基线数据收集完成之前，请勿对任何资源（Assets）进行编辑。

## 2. 阅读项目上下文

阅读并分析以下内容：

- 目标地图中的 Actor 列表
- 项目渲染设置
- 可伸缩性设置（Scalability Settings）
- 设备配置文件（Device Profiles）
- 所有相关的蓝图（Blueprints）
- 所有被引用的材质（Materials）
- 光照及阴影设置
- 后处理体积（Post Process Volumes）
- Niagara 特效系统
- SceneCapture Actor
- 高多边形网格体（High-poly meshes）及其 Nanite 状态

## 3. 嫌疑对象排序

依据确凿证据而非主观猜测，对潜在的问题源进行优先级排序。

请使用以下分类标签：

- Lumen
- 虚拟阴影贴图（Virtual Shadow Maps）
- 阴影（Shadows）
- 材质（Materials）
- 半透明效果（Translucency）
- 过度绘制（Overdraw）
- Nanite
- 后处理（Post Process）
- SceneCapture / RenderTarget
- Niagara
- UMG / Slate UI 系统
- 蓝图导致的渲染状态失效（Render-state invalidation）
- 显存（VRAM） / 纹理流送（Texture Streaming）

## 4. 生成补丁（Patches）

每次仅生成小规模的补丁。

每一个补丁必须包含以下信息：

- 变更原因
- 确切修改的资源列表
- 预期的 GPU 毫秒级性能提升量
- 潜在的视觉效果风险
- 回滚补丁（用于撤销变更）
- 验证方案

在正式应用前，务必先进行“空运行”（Dry-run）测试。

## 5. 验证

应用一组补丁后，请执行以下验证步骤：

1. 编译所有已更改的蓝图。
2. 重新运行相同的性能基准测试。
3. 对比应用补丁前后的 GPU 帧时间数据。
4. 若视觉输出效果可能发生变化，请截取屏幕快照。
5. 如果变更导致视觉质量或性能出现倒退（Regression），请立即停止并回滚。 ## 6. 最终报告

报告内容：

- 基线数据
- 性能瓶颈
- 已实施的变更
- GPU 耗时对比（变更前/后）
- 视觉风险
- 已舍弃的变更
- 下一轮建议方案