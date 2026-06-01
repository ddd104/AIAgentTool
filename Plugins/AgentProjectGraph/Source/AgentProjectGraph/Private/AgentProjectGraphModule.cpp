#include "AgentProjectGraphModule.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "K2Node_CallFunction.h"
#include "MaterialShaderType.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/SoftObjectPath.h"

#define LOCTEXT_NAMESPACE "FAgentProjectGraphModule"

namespace
{
    constexpr const TCHAR* BlueprintCacheDirName = TEXT(".ai/cache/blueprint_ir");
    constexpr const TCHAR* AssetRegistryCacheDirName = TEXT(".ai/cache/asset_registry");
    constexpr const TCHAR* MaterialCacheDirName = TEXT(".ai/cache/material_ir");

    TSharedPtr<FJsonObject> MakeOk()
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetBoolField(TEXT("ok"), true);
        return Json;
    }

    TSharedPtr<FJsonObject> MakeWarningResult(const FString& Warning)
    {
        TSharedPtr<FJsonObject> Json = MakeOk();
        TArray<TSharedPtr<FJsonValue>> Warnings;
        Warnings.Add(MakeShared<FJsonValueString>(Warning));
        Json->SetBoolField(TEXT("success"), false);
        Json->SetArrayField(TEXT("warnings"), Warnings);
        return Json;
    }

    FString JsonToString(const TSharedPtr<FJsonObject>& Object)
    {
        FString Output;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
        FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
        return Output;
    }

    bool JsonFromString(const FString& Text, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
    {
        if (Text.TrimStartAndEnd().IsEmpty())
        {
            OutObject = MakeShared<FJsonObject>();
            return true;
        }

        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
        if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
        {
            OutError = TEXT("Request body is not valid JSON.");
            return false;
        }
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> JsonValues;
        for (const FString& Value : Values)
        {
            JsonValues.Add(MakeShared<FJsonValueString>(Value));
        }
        return JsonValues;
    }

    void AddWarning(TArray<TSharedPtr<FJsonValue>>& Warnings, const FString& Warning)
    {
        Warnings.Add(MakeShared<FJsonValueString>(Warning));
        UE_LOG(LogTemp, Warning, TEXT("AgentProjectGraph: %s"), *Warning);
    }

    FString CacheRoot(const TCHAR* RelativeDir)
    {
        return FPaths::Combine(FPaths::ProjectDir(), RelativeDir);
    }

    bool EnsureDirectory(const FString& Directory, TArray<TSharedPtr<FJsonValue>>& Warnings)
    {
        if (IFileManager::Get().MakeDirectory(*Directory, true))
        {
            return true;
        }

        AddWarning(Warnings, FString::Printf(TEXT("Failed to create output directory: %s"), *Directory));
        return false;
    }

    FString MakeObjectPath(const FString& AssetPath)
    {
        FString Normalized = AssetPath.TrimStartAndEnd();
        if (Normalized.IsEmpty())
        {
            return Normalized;
        }

        const int32 LastSlash = Normalized.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
        const int32 LastDot = Normalized.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
        if (LastDot > LastSlash)
        {
            return Normalized;
        }

        FString Leaf = Normalized.Mid(LastSlash + 1);
        return FString::Printf(TEXT("%s.%s"), *Normalized, *Leaf);
    }

    FString SanitizedFileName(const FString& AssetPath)
    {
        FString Name = AssetPath;
        Name.ReplaceInline(TEXT("/"), TEXT("_"));
        Name.ReplaceInline(TEXT("\\"), TEXT("_"));
        Name.ReplaceInline(TEXT(":"), TEXT("_"));
        Name.ReplaceInline(TEXT("."), TEXT("_"));
        Name.TrimStartAndEndInline();
        if (Name.StartsWith(TEXT("_")))
        {
            Name.RightChopInline(1);
        }
        return Name.IsEmpty() ? TEXT("asset") : Name;
    }

    bool SaveJsonFile(const FString& Directory, const FString& FileName, const TSharedPtr<FJsonObject>& Json, FString& OutPath, TArray<TSharedPtr<FJsonValue>>& Warnings)
    {
        if (!EnsureDirectory(Directory, Warnings))
        {
            return false;
        }

        OutPath = FPaths::Combine(Directory, FileName);
        if (!FFileHelper::SaveStringToFile(JsonToString(Json), *OutPath))
        {
            AddWarning(Warnings, FString::Printf(TEXT("Failed to write JSON: %s"), *OutPath));
            return false;
        }
        return true;
    }

    FString ObjectPathOrEmpty(const UObject* Object)
    {
        return Object ? Object->GetPathName() : FString();
    }

    FString ClassPathOrEmpty(const UClass* Class)
    {
        return Class ? Class->GetPathName() : FString();
    }

    FString EnumValueToString(const UEnum* Enum, int64 Value)
    {
        return Enum ? Enum->GetNameStringByValue(Value) : FString::FromInt(static_cast<int32>(Value));
    }

    IAssetRegistry& AssetRegistry()
    {
        return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    }

    void PackageReferences(FName PackageName, TArray<FString>& OutDependencies, TArray<FString>& OutReferencers)
    {
        IAssetRegistry& Registry = AssetRegistry();

        TArray<FName> Dependencies;
        Registry.GetDependencies(PackageName, Dependencies, UE::AssetRegistry::EDependencyCategory::Package);
        Dependencies.Sort(FNameLexicalLess());
        for (const FName& Dependency : Dependencies)
        {
            OutDependencies.Add(Dependency.ToString());
        }

        TArray<FName> Referencers;
        Registry.GetReferencers(PackageName, Referencers, UE::AssetRegistry::EDependencyCategory::Package);
        Referencers.Sort(FNameLexicalLess());
        for (const FName& Referencer : Referencers)
        {
            OutReferencers.Add(Referencer.ToString());
        }
    }

    TSharedPtr<FJsonObject> AssetSummaryJson(const FAssetData& AssetData)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("assetPath"), AssetData.GetObjectPathString());
        Json->SetStringField(TEXT("assetClass"), AssetData.AssetClassPath.ToString());
        Json->SetStringField(TEXT("packageName"), AssetData.PackageName.ToString());

        TArray<FString> Dependencies;
        TArray<FString> Referencers;
        PackageReferences(AssetData.PackageName, Dependencies, Referencers);
        Json->SetArrayField(TEXT("dependencies"), StringArrayToJson(Dependencies));
        Json->SetArrayField(TEXT("referencers"), StringArrayToJson(Referencers));
        return Json;
    }

    TArray<TSharedPtr<FJsonValue>> GraphSummaryArray(const TArray<TObjectPtr<UEdGraph>>& Graphs)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const UEdGraph* Graph : Graphs)
        {
            if (!Graph)
            {
                continue;
            }

            TSharedPtr<FJsonObject> GraphJson = MakeShared<FJsonObject>();
            GraphJson->SetStringField(TEXT("name"), Graph->GetName());
            GraphJson->SetNumberField(TEXT("nodeCount"), Graph->Nodes.Num());

            TArray<TSharedPtr<FJsonValue>> Nodes;
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node)
                {
                    continue;
                }
                TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
                NodeJson->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
                NodeJson->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
                Nodes.Add(MakeShared<FJsonValueObject>(NodeJson));
            }
            GraphJson->SetArrayField(TEXT("nodes"), Nodes);
            Values.Add(MakeShared<FJsonValueObject>(GraphJson));
        }
        return Values;
    }

    TArray<TSharedPtr<FJsonValue>> CalledFunctionArray(const UBlueprint* Blueprint)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        TSet<FString> Seen;
        TArray<UEdGraph*> Graphs;
        for (UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            Graphs.Add(Graph);
        }
        for (UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            Graphs.Add(Graph);
        }

        for (const UEdGraph* Graph : Graphs)
        {
            if (!Graph)
            {
                continue;
            }

            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
                if (!CallNode)
                {
                    continue;
                }

                const UFunction* Function = CallNode->GetTargetFunction();
                const FString FunctionPath = Function ? Function->GetPathName() : CallNode->FunctionReference.GetMemberName().ToString();
                const FString Key = FString::Printf(TEXT("%s|%s"), *Graph->GetName(), *FunctionPath);
                if (Seen.Contains(Key))
                {
                    continue;
                }
                Seen.Add(Key);

                TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
                Json->SetStringField(TEXT("graph"), Graph->GetName());
                Json->SetStringField(TEXT("function"), FunctionPath);
                Json->SetStringField(TEXT("nodeTitle"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
                Values.Add(MakeShared<FJsonValueObject>(Json));
            }
        }
        return Values;
    }

    TArray<TSharedPtr<FJsonValue>> VariableArray(const UBlueprint* Blueprint)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
        {
            TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("name"), Variable.VarName.ToString());
            Json->SetStringField(TEXT("category"), Variable.Category.ToString());
            Json->SetStringField(TEXT("pinCategory"), Variable.VarType.PinCategory.ToString());
            Json->SetStringField(TEXT("pinSubCategory"), Variable.VarType.PinSubCategory.ToString());
            if (Variable.VarType.PinSubCategoryObject.IsValid())
            {
                Json->SetStringField(TEXT("pinSubCategoryObject"), Variable.VarType.PinSubCategoryObject->GetPathName());
            }
            Values.Add(MakeShared<FJsonValueObject>(Json));
        }
        return Values;
    }

    TArray<TSharedPtr<FJsonValue>> InterfaceArray(const UBlueprint* Blueprint)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
        {
            if (Interface.Interface)
            {
                Values.Add(MakeShared<FJsonValueString>(Interface.Interface->GetPathName()));
            }
        }
        return Values;
    }

    TArray<TSharedPtr<FJsonValue>> ComponentArray(const UBlueprint* Blueprint)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        const USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
        if (!SCS)
        {
            return Values;
        }

        for (const USCS_Node* Node : SCS->GetAllNodes())
        {
            if (!Node)
            {
                continue;
            }

            TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
            Json->SetStringField(TEXT("componentClass"), ClassPathOrEmpty(Node->ComponentClass));
            Values.Add(MakeShared<FJsonValueObject>(Json));
        }
        return Values;
    }

    TSharedPtr<FJsonObject> BlueprintIrJson(UBlueprint* Blueprint, const FString& AssetPath)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("assetPath"), AssetPath);
        Json->SetStringField(TEXT("blueprintName"), Blueprint->GetName());
        Json->SetStringField(TEXT("parentClass"), ClassPathOrEmpty(Blueprint->ParentClass));
        Json->SetStringField(TEXT("generatedClass"), ClassPathOrEmpty(Blueprint->GeneratedClass));
        Json->SetArrayField(TEXT("implementedInterfaces"), InterfaceArray(Blueprint));
        Json->SetArrayField(TEXT("variables"), VariableArray(Blueprint));
        Json->SetArrayField(TEXT("functions"), GraphSummaryArray(Blueprint->FunctionGraphs));
        Json->SetArrayField(TEXT("eventGraphs"), GraphSummaryArray(Blueprint->UbergraphPages));
        Json->SetArrayField(TEXT("components"), ComponentArray(Blueprint));
        Json->SetArrayField(TEXT("calledFunctions"), CalledFunctionArray(Blueprint));

        TArray<FString> Dependencies;
        TArray<FString> Referencers;
        PackageReferences(Blueprint->GetOutermost()->GetFName(), Dependencies, Referencers);
        Json->SetArrayField(TEXT("referencedAssets"), StringArrayToJson(Dependencies));
        return Json;
    }

    TSharedPtr<FJsonObject> MaterialIrJson(UMaterial* Material, const FString& AssetPath)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("assetPath"), AssetPath);
        Json->SetStringField(TEXT("materialDomain"), EnumValueToString(StaticEnum<EMaterialDomain>(), static_cast<int64>(Material->MaterialDomain)));
        Json->SetStringField(TEXT("blendMode"), EnumValueToString(StaticEnum<EBlendMode>(), static_cast<int64>(Material->BlendMode)));
        Json->SetStringField(TEXT("shadingModel"), GetShadingModelFieldString(Material->GetShadingModels()));

        TArray<TSharedPtr<FJsonValue>> Expressions;
        for (const TObjectPtr<UMaterialExpression>& ExpressionPtr : Material->GetExpressions())
        {
            UMaterialExpression* Expression = ExpressionPtr.Get();
            if (!Expression)
            {
                continue;
            }

            TArray<FString> Captions;
            Expression->GetCaption(Captions);

            TSharedPtr<FJsonObject> ExpressionJson = MakeShared<FJsonObject>();
            ExpressionJson->SetStringField(TEXT("name"), Expression->GetName());
            ExpressionJson->SetStringField(TEXT("class"), Expression->GetClass()->GetPathName());
            ExpressionJson->SetNumberField(TEXT("editorX"), Expression->MaterialExpressionEditorX);
            ExpressionJson->SetNumberField(TEXT("editorY"), Expression->MaterialExpressionEditorY);
            ExpressionJson->SetArrayField(TEXT("caption"), StringArrayToJson(Captions));
            ExpressionJson->SetNumberField(TEXT("inputCount"), Expression->CountInputs());
            Expressions.Add(MakeShared<FJsonValueObject>(ExpressionJson));
        }
        Json->SetArrayField(TEXT("expressions"), Expressions);
        return Json;
    }

    UObject* LoadAssetObject(const FString& AssetPath, TArray<TSharedPtr<FJsonValue>>& Warnings)
    {
        const FString ObjectPath = MakeObjectPath(AssetPath);
        if (ObjectPath.IsEmpty())
        {
            AddWarning(Warnings, TEXT("Asset path is empty."));
            return nullptr;
        }

        UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
        if (!Object)
        {
            AddWarning(Warnings, FString::Printf(TEXT("Failed to load asset for read-only export: %s"), *AssetPath));
        }
        return Object;
    }

    bool GetAssetsByClass(UClass* Class, TArray<FAssetData>& OutAssets)
    {
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(TEXT("/Game")));
        Filter.ClassPaths.Add(Class->GetClassPathName());
        Filter.bRecursivePaths = true;
        Filter.bRecursiveClasses = true;
        return AssetRegistry().GetAssets(Filter, OutAssets);
    }
}

void FAgentProjectGraphModule::StartupModule()
{
#if WITH_EDITOR
    if (IsRunningCommandlet())
    {
        return;
    }

    RegisterConsoleCommands();
    StartHttpServer();
#endif
}

void FAgentProjectGraphModule::ShutdownModule()
{
#if WITH_EDITOR
    StopHttpServer();
    UnregisterConsoleCommands();
#endif
}

void FAgentProjectGraphModule::RegisterConsoleCommands()
{
    IConsoleManager& ConsoleManager = IConsoleManager::Get();
    ConsoleCommands.Add(ConsoleManager.RegisterConsoleCommand(
        TEXT("AgentProjectGraph.ExportAll"),
        TEXT("Export read-only Blueprint, asset registry, and material summaries to .ai/cache."),
        FConsoleCommandWithArgsDelegate::CreateRaw(this, &FAgentProjectGraphModule::HandleExportAllCommand),
        ECVF_Default));

    ConsoleCommands.Add(ConsoleManager.RegisterConsoleCommand(
        TEXT("AgentProjectGraph.ExportBlueprint"),
        TEXT("Export one Blueprint summary. Usage: AgentProjectGraph.ExportBlueprint /Game/Path/BP_Name"),
        FConsoleCommandWithArgsDelegate::CreateRaw(this, &FAgentProjectGraphModule::HandleExportBlueprintCommand),
        ECVF_Default));

    ConsoleCommands.Add(ConsoleManager.RegisterConsoleCommand(
        TEXT("AgentProjectGraph.ExportAssets"),
        TEXT("Export AssetRegistry summary to .ai/cache/asset_registry/assets.json."),
        FConsoleCommandWithArgsDelegate::CreateRaw(this, &FAgentProjectGraphModule::HandleExportAssetsCommand),
        ECVF_Default));

    ConsoleCommands.Add(ConsoleManager.RegisterConsoleCommand(
        TEXT("AgentProjectGraph.ExportMaterials"),
        TEXT("Export material summaries to .ai/cache/material_ir."),
        FConsoleCommandWithArgsDelegate::CreateRaw(this, &FAgentProjectGraphModule::HandleExportMaterialsCommand),
        ECVF_Default));
}

void FAgentProjectGraphModule::UnregisterConsoleCommands()
{
    IConsoleManager& ConsoleManager = IConsoleManager::Get();
    for (IConsoleObject* Command : ConsoleCommands)
    {
        if (Command)
        {
            ConsoleManager.UnregisterConsoleObject(Command);
        }
    }
    ConsoleCommands.Reset();
}

void FAgentProjectGraphModule::StartHttpServer()
{
    int32 PortArg = 0;
    if (FParse::Value(FCommandLine::Get(), TEXT("APGPort="), PortArg) && PortArg > 0)
    {
        ListenPort = static_cast<uint32>(PortArg);
    }

    const FString EnvPort = FPlatformMisc::GetEnvironmentVariable(TEXT("APG_BRIDGE_PORT"));
    if (!EnvPort.IsEmpty())
    {
        const int32 Parsed = FCString::Atoi(*EnvPort);
        if (Parsed > 0)
        {
            ListenPort = static_cast<uint32>(Parsed);
        }
    }

    FHttpServerModule& HttpServerModule = FHttpServerModule::Get();
    Router = HttpServerModule.GetHttpRouter(ListenPort);
    BindRoutes();
    HttpServerModule.StartAllListeners();

    UE_LOG(LogTemp, Display, TEXT("AgentProjectGraph bridge listening on http://127.0.0.1:%u"), ListenPort);
}

void FAgentProjectGraphModule::StopHttpServer()
{
    if (!Router.IsValid())
    {
        return;
    }

    for (const FHttpRouteHandle& Handle : RouteHandles)
    {
        Router->UnbindRoute(Handle);
    }
    RouteHandles.Reset();
    Router.Reset();
}

void FAgentProjectGraphModule::BindRoutes()
{
    check(Router.IsValid());

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/agent-project-graph/health")), EHttpServerRequestVerbs::VERB_GET,
        FHttpRequestHandler::CreateRaw(this, &FAgentProjectGraphModule::HandleHealth)));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/agent-project-graph/export-all")), EHttpServerRequestVerbs::VERB_POST,
        FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/agent-project-graph/export-all"), Request, OnComplete, [this](const TSharedPtr<FJsonObject>&)
            {
                return ExportAll();
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/agent-project-graph/export-assets")), EHttpServerRequestVerbs::VERB_POST,
        FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/agent-project-graph/export-assets"), Request, OnComplete, [this](const TSharedPtr<FJsonObject>&)
            {
                return ExportAssets();
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/agent-project-graph/export-blueprint")), EHttpServerRequestVerbs::VERB_POST,
        FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/agent-project-graph/export-blueprint"), Request, OnComplete, [this](const TSharedPtr<FJsonObject>& Body)
            {
                FString AssetPath;
                Body->TryGetStringField(TEXT("assetPath"), AssetPath);
                return ExportBlueprint(AssetPath);
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/agent-project-graph/export-materials")), EHttpServerRequestVerbs::VERB_POST,
        FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/agent-project-graph/export-materials"), Request, OnComplete, [this](const TSharedPtr<FJsonObject>&)
            {
                return ExportAllMaterials();
            });
        })));
}

void FAgentProjectGraphModule::HandleExportAllCommand(const TArray<FString>& Args)
{
    UE_LOG(LogTemp, Display, TEXT("%s"), *JsonToString(ExportAll()));
}

void FAgentProjectGraphModule::HandleExportBlueprintCommand(const TArray<FString>& Args)
{
    if (Args.Num() < 1)
    {
        UE_LOG(LogTemp, Warning, TEXT("Usage: AgentProjectGraph.ExportBlueprint /Game/Path/BP_Name"));
        return;
    }
    UE_LOG(LogTemp, Display, TEXT("%s"), *JsonToString(ExportBlueprint(Args[0])));
}

void FAgentProjectGraphModule::HandleExportAssetsCommand(const TArray<FString>& Args)
{
    UE_LOG(LogTemp, Display, TEXT("%s"), *JsonToString(ExportAssets()));
}

void FAgentProjectGraphModule::HandleExportMaterialsCommand(const TArray<FString>& Args)
{
    UE_LOG(LogTemp, Display, TEXT("%s"), *JsonToString(ExportAllMaterials()));
}

bool FAgentProjectGraphModule::HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    TSharedPtr<FJsonObject> Json = MakeOk();
    Json->SetStringField(TEXT("name"), TEXT("AgentProjectGraph"));
    Json->SetStringField(TEXT("version"), TEXT("0.1.0"));
    Json->SetNumberField(TEXT("port"), ListenPort);
    SendJson(OnComplete, Json);
    return true;
}

bool FAgentProjectGraphModule::HandleJsonRoute(
    const FString& RouteName,
    const FHttpServerRequest& Request,
    const FHttpResultCallback& OnComplete,
    TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonObject>& Body)> Handler)
{
    const FUTF8ToTCHAR BodyChars(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
    const FString BodyText(BodyChars.Length(), BodyChars.Get());
    TSharedPtr<FJsonObject> Body;
    FString Error;
    if (!JsonFromString(BodyText, Body, Error))
    {
        TSharedPtr<FJsonObject> Json = MakeWarningResult(FString::Printf(TEXT("%s: %s"), *RouteName, *Error));
        SendJson(OnComplete, Json, EHttpServerResponseCodes::BadRequest);
        return true;
    }

    TSharedPtr<FJsonObject> Json;
    Json = Handler(Body);

    if (!Json.IsValid())
    {
        Json = MakeWarningResult(FString::Printf(TEXT("%s did not return a JSON result."), *RouteName));
    }

    SendJson(OnComplete, Json);
    return true;
}

void FAgentProjectGraphModule::SendJson(const FHttpResultCallback& OnComplete, const TSharedPtr<FJsonObject>& Json, EHttpServerResponseCodes Code) const
{
    TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(JsonToString(Json), TEXT("application/json"));
    Response->Code = Code;
    OnComplete(MoveTemp(Response));
}

TSharedPtr<FJsonObject> FAgentProjectGraphModule::ExportAll()
{
    TSharedPtr<FJsonObject> Json = MakeOk();
    TArray<TSharedPtr<FJsonValue>> Warnings;

    TSharedPtr<FJsonObject> Assets = ExportAssets();
    TSharedPtr<FJsonObject> Blueprints = ExportAllBlueprints();
    TSharedPtr<FJsonObject> Materials = ExportAllMaterials();

    Json->SetObjectField(TEXT("assets"), Assets);
    Json->SetObjectField(TEXT("blueprints"), Blueprints);
    Json->SetObjectField(TEXT("materials"), Materials);

    auto AppendWarnings = [&Warnings](const TSharedPtr<FJsonObject>& Result)
    {
        const TArray<TSharedPtr<FJsonValue>>* SourceWarnings = nullptr;
        if (Result.IsValid() && Result->TryGetArrayField(TEXT("warnings"), SourceWarnings) && SourceWarnings)
        {
            Warnings.Append(*SourceWarnings);
        }
    };

    AppendWarnings(Assets);
    AppendWarnings(Blueprints);
    AppendWarnings(Materials);
    Json->SetArrayField(TEXT("warnings"), Warnings);
    return Json;
}

TSharedPtr<FJsonObject> FAgentProjectGraphModule::ExportAssets()
{
    TSharedPtr<FJsonObject> Json = MakeOk();
    TArray<TSharedPtr<FJsonValue>> Warnings;
    TArray<TSharedPtr<FJsonValue>> Assets;

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(TEXT("/Game")));
    Filter.bRecursivePaths = true;

    TArray<FAssetData> AssetDataList;
    if (!AssetRegistry().GetAssets(Filter, AssetDataList))
    {
        AddWarning(Warnings, TEXT("AssetRegistry GetAssets returned false."));
    }

    for (const FAssetData& AssetData : AssetDataList)
    {
        Assets.Add(MakeShared<FJsonValueObject>(AssetSummaryJson(AssetData)));
    }

    Json->SetArrayField(TEXT("assets"), Assets);
    Json->SetNumberField(TEXT("assetCount"), Assets.Num());
    Json->SetArrayField(TEXT("warnings"), Warnings);

    FString OutputPath;
    SaveJsonFile(CacheRoot(AssetRegistryCacheDirName), TEXT("assets.json"), Json, OutputPath, Warnings);
    Json->SetStringField(TEXT("outputPath"), OutputPath);
    Json->SetArrayField(TEXT("warnings"), Warnings);
    return Json;
}

TSharedPtr<FJsonObject> FAgentProjectGraphModule::ExportAllBlueprints()
{
    TSharedPtr<FJsonObject> Json = MakeOk();
    TArray<TSharedPtr<FJsonValue>> Warnings;
    TArray<TSharedPtr<FJsonValue>> Outputs;

    TArray<FAssetData> BlueprintAssets;
    if (!GetAssetsByClass(UBlueprint::StaticClass(), BlueprintAssets))
    {
        AddWarning(Warnings, TEXT("AssetRegistry failed to query Blueprint assets."));
    }

    for (const FAssetData& AssetData : BlueprintAssets)
    {
        TSharedPtr<FJsonObject> Result = ExportBlueprint(AssetData.GetObjectPathString());
        Outputs.Add(MakeShared<FJsonValueObject>(Result));
        const TArray<TSharedPtr<FJsonValue>>* ResultWarnings = nullptr;
        if (Result->TryGetArrayField(TEXT("warnings"), ResultWarnings) && ResultWarnings)
        {
            Warnings.Append(*ResultWarnings);
        }
    }

    Json->SetNumberField(TEXT("exportedCount"), Outputs.Num());
    Json->SetArrayField(TEXT("outputs"), Outputs);
    Json->SetArrayField(TEXT("warnings"), Warnings);
    return Json;
}

TSharedPtr<FJsonObject> FAgentProjectGraphModule::ExportBlueprint(const FString& AssetPath)
{
    TArray<TSharedPtr<FJsonValue>> Warnings;
    UObject* Object = LoadAssetObject(AssetPath, Warnings);
    UBlueprint* Blueprint = Cast<UBlueprint>(Object);
    if (!Blueprint)
    {
        FString ClassName = Object ? Object->GetClass()->GetPathName() : TEXT("<null>");
        AddWarning(Warnings, FString::Printf(TEXT("Asset is not a UBlueprint: %s (%s)"), *AssetPath, *ClassName));
        TSharedPtr<FJsonObject> Json = MakeOk();
        Json->SetBoolField(TEXT("success"), false);
        Json->SetStringField(TEXT("assetPath"), AssetPath);
        Json->SetArrayField(TEXT("warnings"), Warnings);
        return Json;
    }

    TSharedPtr<FJsonObject> Ir = BlueprintIrJson(Blueprint, Blueprint->GetPathName());
    FString OutputPath;
    const bool bSaved = SaveJsonFile(
        CacheRoot(BlueprintCacheDirName),
        SanitizedFileName(Blueprint->GetPathName()) + TEXT(".json"),
        Ir,
        OutputPath,
        Warnings);

    TSharedPtr<FJsonObject> Json = MakeOk();
    Json->SetBoolField(TEXT("success"), bSaved);
    Json->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
    Json->SetStringField(TEXT("outputPath"), OutputPath);
    Json->SetObjectField(TEXT("blueprint"), Ir);
    Json->SetArrayField(TEXT("warnings"), Warnings);
    return Json;
}

TSharedPtr<FJsonObject> FAgentProjectGraphModule::ExportAllMaterials()
{
    TSharedPtr<FJsonObject> Json = MakeOk();
    TArray<TSharedPtr<FJsonValue>> Warnings;
    TArray<TSharedPtr<FJsonValue>> Outputs;

    TArray<FAssetData> MaterialAssets;
    if (!GetAssetsByClass(UMaterial::StaticClass(), MaterialAssets))
    {
        AddWarning(Warnings, TEXT("AssetRegistry failed to query Material assets."));
    }

    for (const FAssetData& AssetData : MaterialAssets)
    {
        TSharedPtr<FJsonObject> Result = ExportMaterial(AssetData.GetObjectPathString());
        Outputs.Add(MakeShared<FJsonValueObject>(Result));
        const TArray<TSharedPtr<FJsonValue>>* ResultWarnings = nullptr;
        if (Result->TryGetArrayField(TEXT("warnings"), ResultWarnings) && ResultWarnings)
        {
            Warnings.Append(*ResultWarnings);
        }
    }

    Json->SetNumberField(TEXT("exportedCount"), Outputs.Num());
    Json->SetArrayField(TEXT("outputs"), Outputs);
    Json->SetArrayField(TEXT("warnings"), Warnings);
    return Json;
}

TSharedPtr<FJsonObject> FAgentProjectGraphModule::ExportMaterial(const FString& AssetPath)
{
    TArray<TSharedPtr<FJsonValue>> Warnings;
    UObject* Object = LoadAssetObject(AssetPath, Warnings);
    UMaterial* Material = Cast<UMaterial>(Object);
    if (!Material)
    {
        FString ClassName = Object ? Object->GetClass()->GetPathName() : TEXT("<null>");
        AddWarning(Warnings, FString::Printf(TEXT("Asset is not a UMaterial: %s (%s)"), *AssetPath, *ClassName));
        TSharedPtr<FJsonObject> Json = MakeOk();
        Json->SetBoolField(TEXT("success"), false);
        Json->SetStringField(TEXT("assetPath"), AssetPath);
        Json->SetArrayField(TEXT("warnings"), Warnings);
        return Json;
    }

    TSharedPtr<FJsonObject> Ir = MaterialIrJson(Material, Material->GetPathName());
    FString OutputPath;
    const bool bSaved = SaveJsonFile(
        CacheRoot(MaterialCacheDirName),
        SanitizedFileName(Material->GetPathName()) + TEXT(".json"),
        Ir,
        OutputPath,
        Warnings);

    TSharedPtr<FJsonObject> Json = MakeOk();
    Json->SetBoolField(TEXT("success"), bSaved);
    Json->SetStringField(TEXT("assetPath"), Material->GetPathName());
    Json->SetStringField(TEXT("outputPath"), OutputPath);
    Json->SetObjectField(TEXT("material"), Ir);
    Json->SetArrayField(TEXT("warnings"), Warnings);
    return Json;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAgentProjectGraphModule, AgentProjectGraph)
