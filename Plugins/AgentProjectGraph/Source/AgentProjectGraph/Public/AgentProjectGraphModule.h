#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "HttpRouteHandle.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "Modules/ModuleManager.h"

class FAgentProjectGraphModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    uint32 ListenPort = 31075;
    TSharedPtr<IHttpRouter> Router;
    TArray<FHttpRouteHandle> RouteHandles;
    TArray<IConsoleObject*> ConsoleCommands;

    void RegisterConsoleCommands();
    void UnregisterConsoleCommands();
    void StartHttpServer();
    void StopHttpServer();
    void BindRoutes();

    void HandleExportAllCommand(const TArray<FString>& Args);
    void HandleExportBlueprintCommand(const TArray<FString>& Args);
    void HandleExportAssetsCommand(const TArray<FString>& Args);
    void HandleExportMaterialsCommand(const TArray<FString>& Args);

    bool HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleJsonRoute(
        const FString& RouteName,
        const FHttpServerRequest& Request,
        const FHttpResultCallback& OnComplete,
        TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonObject>& Body)> Handler);

    void SendJson(const FHttpResultCallback& OnComplete, const TSharedPtr<FJsonObject>& Json, EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok) const;

    TSharedPtr<FJsonObject> ExportAll();
    TSharedPtr<FJsonObject> ExportAssets();
    TSharedPtr<FJsonObject> ExportAllBlueprints();
    TSharedPtr<FJsonObject> ExportBlueprint(const FString& AssetPath);
    TSharedPtr<FJsonObject> ExportAllMaterials();
    TSharedPtr<FJsonObject> ExportMaterial(const FString& AssetPath);
};
