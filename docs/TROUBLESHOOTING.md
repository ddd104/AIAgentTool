# Troubleshooting

## ZIP 解压后插件不出现

确认目录是：

```text
YourProject/Plugins/AgentBlueprintTools/AgentBlueprintTools.uplugin
```

不是：

```text
YourProject/Plugins/ue5-agent-blueprint-tools/Plugins/AgentBlueprintTools/...
```

人类最爱把文件夹多套一层，然后怪工具。公平地说，工具也经常值得被怪。

## 编译找不到 HttpServer

确认 `AgentBlueprintTools.Build.cs` 里包含：

```csharp
"HttpServer"
```

并确认你是 Editor target，不是 Game target。

## MCP 连不上 UE

检查：

```powershell
Invoke-RestMethod -Method Get -Uri http://127.0.0.1:31055/v1/health -Headers @{ "x-abt-token" = "change-me-local" }
```

如果失败：

- UE Editor 是否已启动？
- 插件是否启用？
- `ABT_BRIDGE_TOKEN` 是否一致？
- 端口是否被占用？

## Blueprint compile failed

这是正常反馈，不是灾难。读取 response 里的 `compile` 字段，修 patch 后重试。

## 不同 UE5 小版本报 API 签名错误

这是预期风险。把报错集中在以下文件修：

- `ABTBlueprintTools.cpp`
- `ABTLocalBridgeServer.cpp`
- `ABTMaterialTools.cpp`

不要让 agent 扩散式改全项目。扩散是疾病特征，不是工程方法。
