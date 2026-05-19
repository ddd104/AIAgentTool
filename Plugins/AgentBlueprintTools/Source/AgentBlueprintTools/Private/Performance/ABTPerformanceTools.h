#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FABTPerformanceTools
{
public:
    static bool AnalyzeProject(const TSharedPtr<FJsonObject>& Request, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
    static bool DryRunOptimization(const TSharedPtr<FJsonObject>& Request, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
    static bool ApplyOptimization(const TSharedPtr<FJsonObject>& Request, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
};
