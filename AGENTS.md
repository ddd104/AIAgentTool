# UE5 Agent 规则

- 绝不直接编辑 `.uasset` 文件。
- 涉及蓝图（Blueprint）、材质（Material）及资产（Asset）相关工作时，请使用 `ue5_agent_blueprint_tools` MCP 服务器。
- 在修改蓝图之前，务必先调用 `read_blueprint`。
- 在调用 `apply_blueprint_patch` 之前，务必先调用 `dry_run_blueprint_patch` 进行预检。
- 修改蓝图后，务必进行编译。
- 除非用户明确要求或任务确有需要，否则请勿保存资产。
- 优先使用精简的蓝图函数，而非将所有逻辑堆砌在事件图表（EventGraph）中。
- 除非有明确要求，否则应避免使用 Tick 事件。
- 绝不凭空捏造引脚（Pin）名称；请通过查询 Schema、检查 IR 中间表示，或复用现有图表引脚来获取准确名称。
- 优先使用声明式的 Patch DSL 语言，而非直接编写原生 C++ 或 Python 编辑器脚本。
- 若编译失败，请如实报告编译器日志，切勿声称任务已成功完成。

# 目录边界

可作为源码/资料入口：

- `Source/`
- `Plugins/AgentBlueprintTools/`
- `Config/`，仅当任务涉及配置
- `Content/`

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
Unreal_Root = F:\EPIC\Engine\Windows

# 架构设计
阅读 ArchitectureDesign.md