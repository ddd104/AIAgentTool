#include "Bridge/ABTLocalBridgeServer.h"
#include "Blueprint/ABTBlueprintTools.h"
#include "Materials/ABTMaterialTools.h"
#include "Assets/ABTAssetTools.h"
#include "Performance/ABTPerformanceTools.h"
#include "Utils/ABTGameThread.h"
#include "Utils/ABTJson.h"
#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "HttpServerResponse.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Runtime/Launch/Resources/Version.h"

namespace
{
    template <typename LambdaType>
    FHttpRequestHandler MakeHttpHandler(LambdaType&& Lambda)
    {
#if ENGINE_MAJOR_VERSION >= 5
        return FHttpRequestHandler::CreateLambda(Forward<LambdaType>(Lambda));
#else
        return FHttpRequestHandler(Forward<LambdaType>(Lambda));
#endif
    }
}

void FABTLocalBridgeServer::Start()
{
    int32 PortEnv = 0;
    if (FParse::Value(FCommandLine::Get(), TEXT("ABTPort="), PortEnv) && PortEnv > 0)
    {
        ListenPort = static_cast<uint32>(PortEnv);
    }

    const FString EnvPort = FPlatformMisc::GetEnvironmentVariable(TEXT("ABT_BRIDGE_PORT"));
    if (!EnvPort.IsEmpty())
    {
        const int32 Parsed = FCString::Atoi(*EnvPort);
        if (Parsed > 0) ListenPort = static_cast<uint32>(Parsed);
    }

    const FString EnvToken = FPlatformMisc::GetEnvironmentVariable(TEXT("ABT_BRIDGE_TOKEN"));
    if (!EnvToken.IsEmpty())
    {
        ExpectedToken = EnvToken;
    }

    const FString EnvAuditJson = FPlatformMisc::GetEnvironmentVariable(TEXT("ABT_AUDIT_JSON"));
    if (!EnvAuditJson.IsEmpty())
    {
        bAuditJson = !EnvAuditJson.Equals(TEXT("0")) && !EnvAuditJson.Equals(TEXT("false"), ESearchCase::IgnoreCase);
    }

    AuditDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AgentBlueprintTools"), TEXT("Requests"));
    if (bAuditJson)
    {
        IFileManager::Get().MakeDirectory(*AuditDirectory, true);
    }

    FHttpServerModule& HttpServerModule = FHttpServerModule::Get();
    Router = HttpServerModule.GetHttpRouter(ListenPort);
    BindRoutes();
    HttpServerModule.StartAllListeners();

    UE_LOG(LogTemp, Display, TEXT("AgentBlueprintTools bridge listening on http://127.0.0.1:%u"), ListenPort);
    if (bAuditJson)
    {
        UE_LOG(LogTemp, Display, TEXT("AgentBlueprintTools JSON audit directory: %s"), *AuditDirectory);
    }
}

void FABTLocalBridgeServer::Stop()
{
    if (Router.IsValid())
    {
        for (const FHttpRouteHandle& Handle : RouteHandles)
        {
            Router->UnbindRoute(Handle);
        }
        RouteHandles.Reset();
        Router.Reset();
    }
    FHttpServerModule::Get().StopAllListeners();
}

bool FABTLocalBridgeServer::IsAuthorized(const FHttpServerRequest& Request) const
{
    const TArray<FString>* Values = Request.Headers.Find(TEXT("x-abt-token"));
    if (!Values || Values->Num() == 0)
    {
        return false;
    }
    return (*Values)[0] == ExpectedToken;
}

void FABTLocalBridgeServer::SendJson(const FHttpResultCallback& OnComplete, const TSharedPtr<FJsonObject>& Json, EHttpServerResponseCodes Code) const
{
    const FString Body = ABTJson::ToString(Json);
    TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
    Response->Code = Code;
    OnComplete(MoveTemp(Response));
}

bool FABTLocalBridgeServer::HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request))
    {
        SendJson(OnComplete, ABTJson::Error(TEXT("Unauthorized")), EHttpServerResponseCodes::Denied);
        return true;
    }

    TSharedPtr<FJsonObject> Json = ABTJson::Ok();
    Json->SetStringField(TEXT("name"), TEXT("AgentBlueprintTools"));
    Json->SetStringField(TEXT("version"), TEXT("0.1.0"));
    Json->SetNumberField(TEXT("port"), ListenPort);
    SendJson(OnComplete, Json);
    return true;
}

bool FABTLocalBridgeServer::HandleJsonRoute(
    const FString& RouteName,
    const FHttpServerRequest& Request,
    const FHttpResultCallback& OnComplete,
    TFunction<bool(const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutJson, FString& OutError)> Handler)
{
    if (!IsAuthorized(Request))
    {
        SendJson(OnComplete, ABTJson::Error(TEXT("Unauthorized")), EHttpServerResponseCodes::Denied);
        return true;
    }

    const FUTF8ToTCHAR BodyChars(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
    const FString BodyText(BodyChars.Length(), BodyChars.Get());
    TSharedPtr<FJsonObject> Body;
    FString Error;
    if (!ABTJson::FromString(BodyText, Body, Error))
    {
        TSharedPtr<FJsonObject> ErrorJson = ABTJson::Error(Error);
        WriteAuditJson(RouteName, nullptr, ErrorJson, EHttpServerResponseCodes::BadRequest);
        SendJson(OnComplete, ErrorJson, EHttpServerResponseCodes::BadRequest);
        return true;
    }

    TSharedPtr<FJsonObject> OutJson;
    bool bOk = false;
    ABTGameThread::RunSync([&]()
    {
        bOk = Handler(Body, OutJson, Error);
    });

    if (!bOk)
    {
        TSharedPtr<FJsonObject> ErrorJson = ABTJson::Error(Error);
        WriteAuditJson(RouteName, Body, ErrorJson, EHttpServerResponseCodes::BadRequest);
        SendJson(OnComplete, ErrorJson, EHttpServerResponseCodes::BadRequest);
        return true;
    }

    if (!OutJson.IsValid())
    {
        OutJson = ABTJson::Ok();
    }

    WriteAuditJson(RouteName, Body, OutJson, EHttpServerResponseCodes::Ok);
    SendJson(OnComplete, OutJson);
    return true;
}

void FABTLocalBridgeServer::WriteAuditJson(
    const FString& RouteName,
    const TSharedPtr<FJsonObject>& RequestJson,
    const TSharedPtr<FJsonObject>& ResponseJson,
    EHttpServerResponseCodes Code) const
{
    if (!bAuditJson || AuditDirectory.IsEmpty())
    {
        return;
    }

    TSharedPtr<FJsonObject> Audit = ABTJson::Object();
    Audit->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
    Audit->SetStringField(TEXT("route"), RouteName);
    Audit->SetNumberField(TEXT("status_code"), static_cast<int32>(Code));
    if (RequestJson.IsValid())
    {
        Audit->SetObjectField(TEXT("request"), RequestJson);
    }
    if (ResponseJson.IsValid())
    {
        Audit->SetObjectField(TEXT("response"), ResponseJson);
    }

    FString SafeRoute = RouteName;
    SafeRoute.ReplaceInline(TEXT("/"), TEXT("_"));
    SafeRoute.ReplaceInline(TEXT("\\"), TEXT("_"));
    SafeRoute.TrimStartAndEndInline();
    if (SafeRoute.IsEmpty())
    {
        SafeRoute = TEXT("route");
    }

    const FString FileName = FString::Printf(
        TEXT("%s_%s.json"),
        *FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S_%s")),
        *SafeRoute);
    const FString FullPath = FPaths::Combine(AuditDirectory, FileName);
    FFileHelper::SaveStringToFile(ABTJson::ToString(Audit), *FullPath);
}

void FABTLocalBridgeServer::BindRoutes()
{
    check(Router.IsValid());

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/health")), EHttpServerRequestVerbs::VERB_GET,
        MakeHttpHandler(
        [this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleHealth(Request, OnComplete);
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/blueprint/read")), EHttpServerRequestVerbs::VERB_POST,
        MakeHttpHandler(
        [this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/blueprint/read"), Request, OnComplete, [](const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
            {
                FABTBlueprintExportOptions Options;
                Options.bIncludeGraphs = !ABTJson::GetBool(Body, TEXT("summaryOnly"), false);
                Options.bIncludePins = ABTJson::GetBool(Body, TEXT("includePins"), true);
                return FABTBlueprintTools::ExportBlueprint(ABTJson::GetString(Body, TEXT("assetPath")), OutJson, OutError, Options);
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/blueprint/analyze")), EHttpServerRequestVerbs::VERB_POST,
        MakeHttpHandler(
        [this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/blueprint/analyze"), Request, OnComplete, [](const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
            {
                return FABTBlueprintTools::AnalyzeBlueprintGraph(
                    ABTJson::GetString(Body, TEXT("assetPath")),
                    ABTJson::GetString(Body, TEXT("graphName")),
                    OutJson,
                    OutError);
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/blueprint/functions")), EHttpServerRequestVerbs::VERB_POST,
        MakeHttpHandler(
        [this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/blueprint/functions"), Request, OnComplete, [](const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
            {
                return FABTBlueprintTools::ListCallableFunctions(
                    ABTJson::GetString(Body, TEXT("query")),
                    ABTJson::GetInt(Body, TEXT("limit"), 50),
                    OutJson,
                    OutError);
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/blueprint/patch/dry-run")), EHttpServerRequestVerbs::VERB_POST,
        MakeHttpHandler(
        [this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/blueprint/patch/dry-run"), Request, OnComplete, [](const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
            {
                const TSharedPtr<FJsonObject>* Patch = nullptr;
                if (!Body->TryGetObjectField(TEXT("patch"), Patch) || !Patch || !Patch->IsValid())
                {
                    OutError = TEXT("patch object missing");
                    return false;
                }
                return FABTBlueprintTools::DryRunPatch(*Patch, OutJson, OutError);
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/blueprint/patch/apply")), EHttpServerRequestVerbs::VERB_POST,
        MakeHttpHandler(
        [this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/blueprint/patch/apply"), Request, OnComplete, [](const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
            {
                const TSharedPtr<FJsonObject>* Patch = nullptr;
                if (!Body->TryGetObjectField(TEXT("patch"), Patch) || !Patch || !Patch->IsValid())
                {
                    OutError = TEXT("patch object missing");
                    return false;
                }
                return FABTBlueprintTools::ApplyPatch(*Patch, ABTJson::GetBool(Body, TEXT("saveOnSuccess"), false), OutJson, OutError);
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/blueprint/compile")), EHttpServerRequestVerbs::VERB_POST,
        MakeHttpHandler(
        [this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/blueprint/compile"), Request, OnComplete, [](const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
            {
                return FABTBlueprintTools::CompileBlueprintAsset(ABTJson::GetString(Body, TEXT("assetPath")), OutJson, OutError);
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/material/read")), EHttpServerRequestVerbs::VERB_POST,
        MakeHttpHandler(
        [this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/material/read"), Request, OnComplete, [](const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
            {
                return FABTMaterialTools::ReadMaterial(ABTJson::GetString(Body, TEXT("assetPath")), OutJson, OutError);
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/material/patch")), EHttpServerRequestVerbs::VERB_POST,
        MakeHttpHandler(
        [this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/material/patch"), Request, OnComplete, [](const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
            {
                const TSharedPtr<FJsonObject>* Patch = nullptr;
                if (!Body->TryGetObjectField(TEXT("patch"), Patch) || !Patch || !Patch->IsValid())
                {
                    OutError = TEXT("patch object missing");
                    return false;
                }
                return FABTMaterialTools::PatchMaterial(*Patch, ABTJson::GetBool(Body, TEXT("saveOnSuccess"), false), OutJson, OutError);
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/performance/analyze")), EHttpServerRequestVerbs::VERB_POST,
        MakeHttpHandler(
        [this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/performance/analyze"), Request, OnComplete, [](const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
            {
                return FABTPerformanceTools::AnalyzeProject(Body, OutJson, OutError);
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/performance/optimization/dry-run")), EHttpServerRequestVerbs::VERB_POST,
        MakeHttpHandler(
        [this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/performance/optimization/dry-run"), Request, OnComplete, [](const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
            {
                return FABTPerformanceTools::DryRunOptimization(Body, OutJson, OutError);
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/performance/optimization/apply")), EHttpServerRequestVerbs::VERB_POST,
        MakeHttpHandler(
        [this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/performance/optimization/apply"), Request, OnComplete, [](const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
            {
                return FABTPerformanceTools::ApplyOptimization(Body, OutJson, OutError);
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/asset/create")), EHttpServerRequestVerbs::VERB_POST,
        MakeHttpHandler(
        [this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/asset/create"), Request, OnComplete, [](const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
            {
                return FABTAssetTools::CreateAsset(Body, OutJson, OutError);
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/asset/import")), EHttpServerRequestVerbs::VERB_POST,
        MakeHttpHandler(
        [this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/asset/import"), Request, OnComplete, [](const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
            {
                return FABTAssetTools::ImportAssets(Body, OutJson, OutError);
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/asset/read")), EHttpServerRequestVerbs::VERB_POST,
        MakeHttpHandler(
        [this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/asset/read"), Request, OnComplete, [](const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
            {
                return FABTAssetTools::ReadAsset(Body, OutJson, OutError);
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/asset/set-property")), EHttpServerRequestVerbs::VERB_POST,
        MakeHttpHandler(
        [this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/asset/set-property"), Request, OnComplete, [](const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
            {
                return FABTAssetTools::SetAssetProperty(Body, OutJson, OutError);
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/asset/save")), EHttpServerRequestVerbs::VERB_POST,
        MakeHttpHandler(
        [this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/asset/save"), Request, OnComplete, [](const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
            {
                return FABTAssetTools::SaveAsset(ABTJson::GetString(Body, TEXT("assetPath")), OutJson, OutError);
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/asset/delete")), EHttpServerRequestVerbs::VERB_POST,
        MakeHttpHandler(
        [this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/asset/delete"), Request, OnComplete, [](const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
            {
                return FABTAssetTools::DeleteAsset(ABTJson::GetString(Body, TEXT("assetPath")), OutJson, OutError);
            });
        })));

    RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/v1/level/place-actor")), EHttpServerRequestVerbs::VERB_POST,
        MakeHttpHandler(
        [this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
        {
            return HandleJsonRoute(TEXT("/v1/level/place-actor"), Request, OnComplete, [](const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
            {
                return FABTAssetTools::PlaceActor(Body, OutJson, OutError);
            });
        })));
}
