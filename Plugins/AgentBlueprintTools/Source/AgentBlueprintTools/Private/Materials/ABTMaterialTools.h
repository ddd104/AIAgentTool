#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FABTMaterialTools
{
public:
    static bool ReadMaterial(const FString& AssetPath, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
    static bool PatchMaterial(const TSharedPtr<FJsonObject>& Patch, bool bSaveOnSuccess, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
};
