#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace ABTJson
{
    TSharedPtr<FJsonObject> Ok();
    TSharedPtr<FJsonObject> Error(const FString& Message);
    FString ToString(const TSharedPtr<FJsonObject>& Object);
    bool FromString(const FString& Text, TSharedPtr<FJsonObject>& OutObject, FString& OutError);

    FString GetString(const TSharedPtr<FJsonObject>& Object, const FString& Field, const FString& DefaultValue = TEXT(""));
    bool GetBool(const TSharedPtr<FJsonObject>& Object, const FString& Field, bool DefaultValue = false);
    int32 GetInt(const TSharedPtr<FJsonObject>& Object, const FString& Field, int32 DefaultValue = 0);
    double GetNumber(const TSharedPtr<FJsonObject>& Object, const FString& Field, double DefaultValue = 0.0);

    TSharedPtr<FJsonObject> Object();
    TSharedPtr<FJsonValue> StringValue(const FString& Value);
    TSharedPtr<FJsonValue> ObjectValue(const TSharedPtr<FJsonObject>& Value);
}
