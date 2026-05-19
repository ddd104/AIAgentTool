#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FABTAssetTools
{
public:
    static bool CreateAsset(const TSharedPtr<FJsonObject>& Request, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
    static bool ImportAssets(const TSharedPtr<FJsonObject>& Request, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
    static bool ReadAsset(const TSharedPtr<FJsonObject>& Request, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
    static bool SetAssetProperty(const TSharedPtr<FJsonObject>& Request, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
    static bool SaveAsset(const FString& AssetPath, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
    static bool DeleteAsset(const FString& AssetPath, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
    static bool PlaceActor(const TSharedPtr<FJsonObject>& Request, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
};
