# UE4.27 Agent Blueprint Tools

- 涉及 Blueprint、Material、Content Browser Asset 或关卡 Actor 放置时，使用 `ue5-blueprint-agent` skill 和 `ue5_agent_blueprint_tools` MCP server；名称沿用兼容配置，但本项目按 UE4.27 使用。
- 绝不直接编辑 `.uasset`、`.umap` 或其他二进制资源文件。
- 修改 Blueprint 前必须先调用 `read_blueprint`；涉及具体图表时再调用 `analyze_blueprint_graph`。
- 应用 Blueprint patch 前必须先调用 `dry_run_blueprint_patch`；dry-run 成功后才可调用 `apply_blueprint_patch`。
- 修改 Blueprint 后必须检查编译结果；编译失败时如实报告日志，不要声称完成。
- 除非用户明确要求或任务确有需要，否则不要保存资产或关卡；写入工具的 `saveOnSuccess`、`saveAssets`、`saveLevel` 默认保持 `false`。
- 优先使用精简 Blueprint 函数，不把所有逻辑堆在 EventGraph；除非明确要求，避免新增 Tick。
- 不凭空猜 pin 名称；通过 `read_blueprint`、`analyze_blueprint_graph`、`list_callable_functions` 或现有 IR 获取准确名称。
- 优先使用声明式 Patch DSL；只有当插件/MCP 不支持且任务明确需要时，才考虑 C++ 扩展。

# Figma 转 UMG 流程

- 同时读取 Figma 布局节点和实际控件节点；按源分辨率到 UMG 分辨率做等比缩放，必要时保留居中/边距偏移，避免单独拉伸 X/Y。
- 遇到重复布局时，先创建可复用子 Widget Blueprint；如果差异只是图标、文字、图片等内容，必须把完整布局放在子 Widget 内，把差异内容做成 `EditAnywhere/ExposeOnSpawn` 变量，由父 Widget 的 `UserWidget` 实例设置变量，不要把图标/文字拆回父 Widget 平铺。
- 当前 Blueprint DSL 无法稳定设置子 Widget 实例变量时，可以补一个极薄 C++ `UUserWidget` 父类承载公开变量，并在 `NativePreConstruct` 中应用到内部命名控件；仍需通过 Blueprint 工具创建/配置 Widget Blueprint。
- 图标必须先检查 Figma 子层级；如果包含多个绘制元素、布尔/蒙版结构，或混有 text 等非纯图标元素，按实际语义拆分：文字进 `TextBlock`，复合图标整体合成为单张 PNG，不要只取其中一个子元素。
- Figma 中作为视觉外观的背景、底板、装饰图等不得用 UE 默认 `Border`/纯色替代；应按 Figma 图层导出/合成 PNG 后导入为 Texture，再在 UMG 中以 `Image` 使用。
- 导入 UE 的图片资产必须是 PNG；下载、SVG、截图、合成 PNG、JSON 等中间文件放到 `Content/` 之外的临时目录。
- PNG在导入UE后,texture的 Compression Settings需要设置成Userinterface2D (RGBA), Texture Group需要设置成UI
- 图片导入后只在 `Content/` 下保留 `.uasset` 资产，任务结束前删除残留的 `.png`、`.svg`、`.html`、`.json` 等中间文件。
- 生成或更新 UMG 仍遵循 Blueprint 写入流程：`read_blueprint`、dry-run patch、apply patch、检查编译结果；确需落盘交付时才保存资产。

# 目录边界

可作为源码/资料入口：

- `Source/`
- `Plugins/AgentBlueprintTools/`
- `Config/`，仅当任务涉及配置
- `Content/`

# 代码入口参考

- `CODE_MAP.md`

默认不要阅读、修改或提交：

- `Binaries/`
- `Intermediate/`
- `DerivedDataCache/`
- `Saved/`
- `ArchivedBuilds/`
- `Releases/`
- `Platforms/`
- `Build/`
- `.vs/`
- `.idea/`
- `*.sln`
- `*.user`
- `*.log`
- `*.tmp`
- `*.cache`
- `*.sqlite`
- `*.db`
- `node_modules/`
- `.venv/`
- `dist/`
- `build/`
- `.next/`
- `coverage/`
- `generated/`

# 引擎路径
优先使用环境变量 `UE5_ROOT`；本机验证使用 `F:\EPIC\Engine\Windows`。

# 架构设计
阅读 ArchitectureDesign.md
