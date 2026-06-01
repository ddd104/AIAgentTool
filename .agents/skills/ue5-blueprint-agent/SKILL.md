---
name: ue5-blueprint-agent
description: 当需要通过本地 UE5 MCP 桥接修改 Unreal Engine 5 的蓝图（Blueprints）、蓝图变量、蓝图函数、材质（Materials）、材质参数或内容浏览器（Content Browser）资产时，请使用此技能。
---

# UE4.27 蓝图代理技能

请使用 `ue5_agent_blueprint_tools` MCP 服务器。该软件包或旧版配置也可能被称为 `ue5-agent-blueprint-tools-mcp`；这两个名称均指向同一个 AgentBlueprintTools MCP 桥接。切勿直接编辑 `.uasset` 文件。

## 蓝图读取流程

1. 调用 `read_blueprint(assetPath)`。
2. 如果任务中提及了特定的图表（Graph）或函数，则调用 `analyze_blueprint_graph(assetPath, graphName)`。
3. 总结执行路径、读取/写入的变量、涉及的组件以及外部函数调用。

## 蓝图写入流程

1. 首先调用 `read_blueprint`。
2. 如果需要进行函数调用，则调用 `list_callable_functions(query)`。
3. 生成一个最小化的 YAML 补丁（Patch）。
4. 调用 `dry_run_blueprint_patch` 进行预演（Dry-run）。
5. 仅当预演成功时，才调用 `apply_blueprint_patch` 应用补丁。
6. 检查编译结果。
7. 如果编译失败，请报告日志信息，且不进行保存。

## 补丁编写原则

- 使用小型函数。
- 在添加节点之前，优先使用显式的 `ensure_variable` 和 `ensure_function`。
- 不要凭空捏造引脚（Pin）名称。
- 保持图表编辑的改动尽可能小且可逆。
- 除非任务明确要求，否则避免修改 Tick 事件。 ## 支持的蓝图修补操作

- `ensure_variable`（确保变量存在）
- `ensure_function`（确保函数存在）
- `ensure_move_function`（确保函数移动到位）
- `ensure_component`（确保组件存在）
- `set_static_mesh`（设置静态网格体）
- `set_component_material`（设置组件材质）
- `configure_button_pages_widget`（配置按钮分页控件）
- `configure_figma_widget`（配置 Figma 控件）
- `add_node`（添加节点）
- `set_pin_default`（设置引脚默认值）
- `connect_exec`（连接执行流）
- `connect_data`（连接数据流）
- `remove_node`（移除节点）
- `set_blueprint_property`（设置蓝图属性）

## 材质处理流程

1. 首先调用 `read_material`。
2. 利用返回的材质输出、可达节点计数、表达式输入/输出、纹理采样、函数调用及关键参数，在编辑逻辑之前先归纳材质的意图。
3. 保留所需的混合模式和视觉契约；除非用户明确接受该视觉变化，否则不要将半透明 VFX 材质切换为不透明或遮罩模式。
4. 优先选择替换或绕过特定高开销的可达分支，而非重建整个图表。仅当旧分支作为未连接的参考仍有价值时才予以保留；仅删除已知为死代码的表达式。
5. 使用 `patch_material` 并传入小范围的材质修补内容。
6. 编辑完成后重新编译材质。

## 性能优化流程

针对 Android、Windows 或 Linux 目标平台进行检查时，首先使用 `analyze_performance`。在调用 `apply_performance_optimization` 之前，请先运行 `dry_run_performance_optimization`，以便对生成的优化方案进行审查。除非用户明确要求保存资产，否则请将 `saveAssets` 参数设为 `false`；除非用户接受可能改变外观的材质修改，否则请将 `applyVisualChanges` 参数设为 `false`。
若仅针对特定资产进行操作，请传入 `assetPaths: ["/Game/.../AssetName"]` 参数，而非扫描整个 `/Game` 目录。

## 资产处理流程

请使用 `create_asset`、`import_assets`、`read_asset`、`set_asset_property`、`save_asset` 和 `delete_asset` 等接口，而非直接进行原始文件写入操作。仅删除已知的临时资产，或用户明确要求移除的资产。

针对 UMG 控件，请调用 `create_asset` 并指定 `assetType: WidgetBlueprint` 参数。对于按钮列表弹出页面控件，建议优先使用 `configure_button_pages_widget`，而非手动构建 UMG 控件树及按钮逻辑图。

对于源自 Figma 的 UMG 控件，请使用 `configure_figma_widget`。请在嵌套的 `root.children` 结构中保留 Figma 原有的层级关系。建议首先创建可复用的子控件蓝图（Widget Blueprints）并进行编译，随后在主控件中通过指定 `type: "UserWidget"` 并配合 `widgetAsset` 属性来引用这些子控件。请将 Figma 中的图标或图像（PNG 格式）下载至本地，利用 `import_assets` 将其导入项目，并通过 `brushPath`、`texturePath` 或 `imagePath` 属性来指定导入后生成的纹理资产路径。

请使用 `place_actor` 命令将 Actor 蓝图或 Actor 类放置到当前编辑器关卡中。除非用户明确要求保存关卡，否则请将 `saveLevel` 参数保持为 `false`。

## JSON 审计追踪

当环境变量 `ABT_AUDIT_JSON` 未设置或其值为“真”（truthy）时，Bridge 的请求与响应数据将被写入 `Saved/AgentBlueprintTools/Requests/` 目录下。若需禁用此本地审计追踪功能，请将 `ABT_AUDIT_JSON` 设置为 `0`。