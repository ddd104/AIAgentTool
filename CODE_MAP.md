# AIAgentTool Code Map

更新时间：2026-05-30

本文件用于后续快速检索代码和蓝图职责。蓝图信息通过 `AgentBlueprintTools` 本地桥接读取摘要，未直接编辑或解析 `.uasset` / `.umap` 二进制文件。

## 总览



## C++ 源码目录

### `Source/`



### `Source/Core/`


## AgentBlueprintTools 插件目录

- `Plugins/AgentBlueprintTools/AgentBlueprintTools.uplugin`：插件描述，项目中已启用。
- `Source/AgentBlueprintTools/Public/AgentBlueprintToolsModule.h`、`Private/AgentBlueprintToolsModule.cpp`：插件模块入口。非 Commandlet 模式启动本地桥接服务。
- `Private/Bridge/`：本地 HTTP 桥接服务，默认 `127.0.0.1:31055`，默认 token 为 `change-me-local`。路由包含蓝图读取/分析/patch/编译、材质读取/patch、资产创建/导入/读取/属性设置/保存/删除、关卡 Actor 放置、性能分析和优化。
- `Private\Blueprint/`：蓝图导出、图分析、函数查询、patch 校验、dry-run、apply 和编译。
- `Private\Blueprint/Ops/`：蓝图 patch 操作实现，包含 Actor、Graph、Widget 操作；UMG/Figma 相关逻辑主要在 `ABTWidgetBlueprintOps.*`。
- `Private\Blueprint/Utils/`：蓝图图节点、Pin、SCS 等工具函数。
- `Private\Assets/`：资产创建、导入、读取、设置属性、保存、删除、Actor 放置。
- `Private/Materials/`：材质读取、材质图 patch、材质实例参数处理。
- `Private/Performance/`：项目性能扫描、dry-run 优化计划和应用优化。
- `Private/Commandlets/`：`ABTApplyPatchCommandlet`，支持命令行执行蓝图 patch、材质读取/patch、性能分析等。
- `Private/Tests/`：插件自动化测试，覆盖 patch shape、蓝图/UMG/Figma widget、材质和资产 round-trip。

## Config 入口

- `Config/DefaultEngine.ini`：默认 GameMode、渲染/移动端配置、类/属性重定向；默认 GameMode 指向 `/Game/Blueprint/Core/BP_GameMode`。
- `Config/DefaultGameplayTags.ini`：项目 GameplayTag 配置。
- `Config/DefaultInput.ini`：输入绑定。
- `Config/DefaultDeviceProfiles.ini`：设备 Profile。
- `Config/DefaultPakFileRules.ini`：Pak 规则。
- `Config/Localization/`：本地化 gather/import/export/report 命令配置。

## Content 蓝图与资产地图

### 顶层资产分布


### 常见资产前缀

- `BP_`：Actor、Subsystem、事件处理、数据序列化等蓝图。
- `WBP_`：UMG Widget Blueprint。
- `DT_`：DataTable。
- `M_`、`MI_`、`MT_`、`MPC_`：材质、材质实例、材质模板/测试材质、材质参数集合。
- `SM_`、`SK_`：静态网格、骨骼网格。
- `T_`：贴图。
- `C_`、`CA_`：曲线、Curve Atlas。
- `RT_`：Render Target。

### `Content/Blueprint/`

- `Core/`：公共运行核心蓝图。重点包括 `BP_CarDataSubSystem`、`BP_GameInstance`、`BP_HUD`、`BP_RecordSubsystem`、


### 关键地图


## 已抽读的关键蓝图摘要

| 蓝图 | 父类 | 主要用途 |
| --- | --- | --- |
| `/Game/BluePrint/UMG/NewWidgetBlueprint` | `/Script/UMG.UserWidget` |  UMG 测试/新建 Widget，事件入口含 PreConstruct、Construct、Tick。 |

## 推荐检索路径
- 查构建打包变体：先看 `Config/DefaultEditor.ini` 的 `BuildConfigs`，再根据 `DefaultMap`、`ProjectTags`、`UsedSubsystems` 回到对应蓝图目录。
- 查蓝图图结构：使用 `AgentBlueprintTools` 的 `read_blueprint(assetPath)`；涉及具体函数/图时再用 `analyze_blueprint_graph(assetPath, graphName)`。

## 常用命令

```powershell
# 搜 C++ 符号
rg -n "SetMode|EainMode|" Source/

# 列出蓝图资产路径
rg --files Content/ Content/ -g "*.uasset" -g "*.umap" | rg "Blue[Pp]rint|BluePrint|UMG|Maps"

# 探测 AgentBlueprintTools 桥接服务
Invoke-RestMethod -Method Get -Uri "http://127.0.0.1:31055/v1/health" -Headers @{ "x-abt-token"="change-me-local" }

# 读取蓝图摘要，不直接编辑 .uasset
$body = @{ assetPath="/Game/BluePrint/Core/BP_GameMode"; summaryOnly=$true; includePins=$false } | ConvertTo-Json
Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:31055/v1/blueprint/read" -Headers @{ "x-abt-token"="change-me-local" } -Body $body -ContentType "application/json"
```

## 注意事项

- 不要直接修改 `.uasset`、`.umap` 等二进制资源；蓝图/UMG/材质变更应通过 `AgentBlueprintTools` 的读取、dry-run、apply、编译检查流程。
- 默认不要检索或提交 `Binaries/`、`Intermediate/`、`DerivedDataCache/`、`Saved/`、`.vs/`、`.idea/` 等生成目录。
- `Config/DefaultEditor.ini` 的 `UsedSubsystems` 能快速判断某个包实际启用哪些蓝图子系统，是追调用链的高价值入口。
