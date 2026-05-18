#include "Utils/ABTJson.h"

namespace ABTJson
{
    TSharedPtr<FJsonObject> Ok()
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetBoolField(TEXT("ok"), true);
        return Json;
    }

    TSharedPtr<FJsonObject> Error(const FString& Message)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetBoolField(TEXT("ok"), false);
        Json->SetStringField(TEXT("error"), Message);
        return Json;
    }

    FString ToString(const TSharedPtr<FJsonObject>& Object)
    {
        FString Output;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
        FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
        return Output;
    }

    bool FromString(const FString& Text, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
    {
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
        if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
        {
            OutError = TEXT("Invalid JSON body");
            return false;
        }
        return true;
    }

    FString GetString(const TSharedPtr<FJsonObject>& Object, const FString& Field, const FString& DefaultValue)
    {
        if (!Object.IsValid()) return DefaultValue;
        FString Value;
        return Object->TryGetStringField(Field, Value) ? Value : DefaultValue;
    }

    bool GetBool(const TSharedPtr<FJsonObject>& Object, const FString& Field, bool DefaultValue)
    {
        if (!Object.IsValid()) return DefaultValue;
        bool Value = false;
        return Object->TryGetBoolField(Field, Value) ? Value : DefaultValue;
    }

    int32 GetInt(const TSharedPtr<FJsonObject>& Object, const FString& Field, int32 DefaultValue)
    {
        if (!Object.IsValid()) return DefaultValue;
        int32 Value = 0;
        return Object->TryGetNumberField(Field, Value) ? Value : DefaultValue;
    }

    double GetNumber(const TSharedPtr<FJsonObject>& Object, const FString& Field, double DefaultValue)
    {
        if (!Object.IsValid()) return DefaultValue;
        double Value = 0.0;
        return Object->TryGetNumberField(Field, Value) ? Value : DefaultValue;
    }

    TSharedPtr<FJsonObject> Object()
    {
        return MakeShared<FJsonObject>();
    }

    TSharedPtr<FJsonValue> StringValue(const FString& Value)
    {
        return MakeShared<FJsonValueString>(Value);
    }

    TSharedPtr<FJsonValue> ObjectValue(const TSharedPtr<FJsonObject>& Value)
    {
        return MakeShared<FJsonValueObject>(Value);
    }
}
