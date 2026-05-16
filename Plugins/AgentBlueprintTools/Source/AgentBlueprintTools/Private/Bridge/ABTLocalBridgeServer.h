#pragma once

#include "CoreMinimal.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpRouteHandle.h"

class FABTLocalBridgeServer
{
public:
    void Start();
    void Stop();

private:
    uint32 ListenPort = 31055;
    FString ExpectedToken = TEXT("change-me-local");
    bool bAuditJson = true;
    FString AuditDirectory;
    TSharedPtr<IHttpRouter> Router;
    TArray<FHttpRouteHandle> RouteHandles;

    bool IsAuthorized(const FHttpServerRequest& Request) const;
    bool HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleJsonRoute(
        const FString& RouteName,
        const FHttpServerRequest& Request,
        const FHttpResultCallback& OnComplete,
        TFunction<bool(const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutJson, FString& OutError)> Handler);

    void BindRoutes();
    void SendJson(const FHttpResultCallback& OnComplete, const TSharedPtr<FJsonObject>& Json, EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok) const;
    void WriteAuditJson(const FString& RouteName, const TSharedPtr<FJsonObject>& RequestJson, const TSharedPtr<FJsonObject>& ResponseJson, EHttpServerResponseCodes Code) const;
};
