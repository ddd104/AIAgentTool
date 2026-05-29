#include "Commandlets/ABTApplyPatchCommandlet.h"

#include "Blueprint/ABTBlueprintTools.h"
#include "Materials/ABTMaterialTools.h"
#include "Performance/ABTPerformanceTools.h"
#include "Utils/ABTJson.h"

#include "Misc/FileHelper.h"
#include "Misc/Parse.h"

namespace
{
    bool LoadJsonObjectFile(const FString& Path, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
    {
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *Path))
        {
            OutError = FString::Printf(TEXT("Could not read json file: %s"), *Path);
            return false;
        }

        return ABTJson::FromString(Text, OutJson, OutError);
    }

    bool LoadRequestFromParam(const FString& Params, TSharedPtr<FJsonObject>& OutRequest, FString& OutError)
    {
        FString RequestFile;
        if (!FParse::Value(*Params, TEXT("RequestFile="), RequestFile) || RequestFile.IsEmpty())
        {
            OutRequest = ABTJson::Object();
        }
        else if (!LoadJsonObjectFile(RequestFile, OutRequest, OutError))
        {
            return false;
        }

        FString AssetPath;
        if (FParse::Value(*Params, TEXT("AssetPath="), AssetPath) && !AssetPath.IsEmpty())
        {
            TArray<TSharedPtr<FJsonValue>> AssetPaths;
            AssetPaths.Add(ABTJson::StringValue(AssetPath));
            OutRequest->SetArrayField(TEXT("assetPaths"), AssetPaths);
        }

        FString ContentPath;
        if (FParse::Value(*Params, TEXT("ContentPath="), ContentPath) && !ContentPath.IsEmpty())
        {
            TArray<TSharedPtr<FJsonValue>> ContentPaths;
            ContentPaths.Add(ABTJson::StringValue(ContentPath));
            OutRequest->SetArrayField(TEXT("contentPaths"), ContentPaths);
        }

        FString TargetPlatforms;
        if (FParse::Value(*Params, TEXT("TargetPlatforms="), TargetPlatforms) && !TargetPlatforms.IsEmpty())
        {
            TArray<FString> PlatformNames;
            TargetPlatforms.ParseIntoArray(PlatformNames, TEXT(","), true);

            TArray<TSharedPtr<FJsonValue>> PlatformValues;
            for (FString& PlatformName : PlatformNames)
            {
                PlatformName.TrimStartAndEndInline();
                if (!PlatformName.IsEmpty())
                {
                    PlatformValues.Add(ABTJson::StringValue(PlatformName));
                }
            }
            OutRequest->SetArrayField(TEXT("targetPlatforms"), PlatformValues);
        }

        FString Profile;
        if (FParse::Value(*Params, TEXT("Profile="), Profile) && !Profile.IsEmpty())
        {
            OutRequest->SetStringField(TEXT("profile"), Profile);
        }

        int32 MaxAssets = 0;
        if (FParse::Value(*Params, TEXT("MaxAssets="), MaxAssets) && MaxAssets > 0)
        {
            OutRequest->SetNumberField(TEXT("maxAssets"), MaxAssets);
        }

        int32 MaxActions = 0;
        if (FParse::Value(*Params, TEXT("MaxActions="), MaxActions) && MaxActions > 0)
        {
            OutRequest->SetNumberField(TEXT("maxActions"), MaxActions);
        }

        if (FParse::Param(*Params, TEXT("AllowVisualChanges")))
        {
            OutRequest->SetBoolField(TEXT("allowVisualChanges"), true);
        }

        if (FParse::Param(*Params, TEXT("ApplyVisualChanges")))
        {
            OutRequest->SetBoolField(TEXT("applyVisualChanges"), true);
        }

        if (FParse::Param(*Params, TEXT("SaveAssets")))
        {
            OutRequest->SetBoolField(TEXT("saveAssets"), true);
        }

        if (FParse::Param(*Params, TEXT("NoWriteConfig")))
        {
            OutRequest->SetBoolField(TEXT("writeConfig"), false);
        }

        return true;
    }
}

UABTApplyPatchCommandlet::UABTApplyPatchCommandlet()
{
    IsClient = false;
    IsEditor = true;
    LogToConsole = true;
}

int32 UABTApplyPatchCommandlet::Main(const FString& Params)
{
    FString Mode;
    FParse::Value(*Params, TEXT("Mode="), Mode);
    if (Mode.IsEmpty())
    {
        Mode = TEXT("blueprint-patch");
    }

    if (Mode.Equals(TEXT("material-read"), ESearchCase::IgnoreCase))
    {
        FString AssetPath;
        if (!FParse::Value(*Params, TEXT("AssetPath="), AssetPath) || AssetPath.IsEmpty())
        {
            UE_LOG(LogTemp, Error, TEXT("material-read requires -AssetPath=<asset path>"));
            return 1;
        }

        TSharedPtr<FJsonObject> ResultJson;
        FString Error;
        if (!FABTMaterialTools::ReadMaterial(AssetPath, ResultJson, Error))
        {
            UE_LOG(LogTemp, Error, TEXT("%s"), *Error);
            return 1;
        }

        UE_LOG(LogTemp, Display, TEXT("%s"), *ABTJson::ToString(ResultJson));
        return 0;
    }

    if (Mode.Equals(TEXT("material-settings"), ESearchCase::IgnoreCase))
    {
        FString AssetPath;
        if (!FParse::Value(*Params, TEXT("AssetPath="), AssetPath) || AssetPath.IsEmpty())
        {
            UE_LOG(LogTemp, Error, TEXT("material-settings requires -AssetPath=<asset path>"));
            return 1;
        }

        TSharedPtr<FJsonObject> Op = ABTJson::Object();
        Op->SetStringField(TEXT("op"), TEXT("set_material_settings"));

        FString BlendMode;
        if (FParse::Value(*Params, TEXT("BlendMode="), BlendMode) && !BlendMode.IsEmpty())
        {
            Op->SetStringField(TEXT("blendMode"), BlendMode);
        }

        FString ShadingModel;
        if (FParse::Value(*Params, TEXT("ShadingModel="), ShadingModel) && !ShadingModel.IsEmpty())
        {
            Op->SetStringField(TEXT("shadingModel"), ShadingModel);
        }

        if (FParse::Param(*Params, TEXT("TwoSided")))
        {
            Op->SetBoolField(TEXT("twoSided"), true);
        }
        if (FParse::Param(*Params, TEXT("NoTwoSided")))
        {
            Op->SetBoolField(TEXT("twoSided"), false);
        }
        if (FParse::Param(*Params, TEXT("FullyRough")))
        {
            Op->SetBoolField(TEXT("fullyRough"), true);
        }
        if (FParse::Param(*Params, TEXT("NotFullyRough")))
        {
            Op->SetBoolField(TEXT("fullyRough"), false);
        }
        if (FParse::Param(*Params, TEXT("NoRayTracedShadows")))
        {
            Op->SetBoolField(TEXT("castRayTracedShadows"), false);
        }
        if (FParse::Param(*Params, TEXT("RayTracedShadows")))
        {
            Op->SetBoolField(TEXT("castRayTracedShadows"), true);
        }
        if (FParse::Param(*Params, TEXT("NoTranslucencyVertexFog")))
        {
            Op->SetBoolField(TEXT("translucencyVertexFog"), false);
        }
        if (FParse::Param(*Params, TEXT("TranslucencyVertexFog")))
        {
            Op->SetBoolField(TEXT("translucencyVertexFog"), true);
        }
        if (FParse::Param(*Params, TEXT("NoSeparateTranslucency")))
        {
            Op->SetBoolField(TEXT("separateTranslucency"), false);
        }
        if (FParse::Param(*Params, TEXT("SeparateTranslucency")))
        {
            Op->SetBoolField(TEXT("separateTranslucency"), true);
        }
        if (FParse::Param(*Params, TEXT("NoResponsiveAA")))
        {
            Op->SetBoolField(TEXT("responsiveAA"), false);
        }
        if (FParse::Param(*Params, TEXT("ResponsiveAA")))
        {
            Op->SetBoolField(TEXT("responsiveAA"), true);
        }
        if (FParse::Param(*Params, TEXT("NoTranslucentVelocity")))
        {
            Op->SetBoolField(TEXT("outputTranslucentVelocity"), false);
        }
        if (FParse::Param(*Params, TEXT("TranslucentVelocity")))
        {
            Op->SetBoolField(TEXT("outputTranslucentVelocity"), true);
        }

        TArray<TSharedPtr<FJsonValue>> Ops;
        Ops.Add(ABTJson::ObjectValue(Op));

        TSharedPtr<FJsonObject> Patch = ABTJson::Object();
        Patch->SetStringField(TEXT("target"), AssetPath);
        Patch->SetArrayField(TEXT("operations"), Ops);

        TSharedPtr<FJsonObject> ResultJson;
        FString Error;
        const bool bSave = FParse::Param(*Params, TEXT("Save")) || FParse::Param(*Params, TEXT("SaveAssets"));
        if (!FABTMaterialTools::PatchMaterial(Patch, bSave, ResultJson, Error))
        {
            UE_LOG(LogTemp, Error, TEXT("%s"), *Error);
            return 1;
        }

        UE_LOG(LogTemp, Display, TEXT("%s"), *ABTJson::ToString(ResultJson));
        return 0;
    }

    if (Mode.Equals(TEXT("material-patch"), ESearchCase::IgnoreCase))
    {
        FString PatchFile;
        if (!FParse::Value(*Params, TEXT("PatchFile="), PatchFile) || PatchFile.IsEmpty())
        {
            UE_LOG(LogTemp, Error, TEXT("material-patch requires -PatchFile=<json path>"));
            return 1;
        }

        TSharedPtr<FJsonObject> Body;
        FString Error;
        if (!LoadJsonObjectFile(PatchFile, Body, Error))
        {
            UE_LOG(LogTemp, Error, TEXT("%s"), *Error);
            return 1;
        }

        TSharedPtr<FJsonObject> Patch = Body;
        const TSharedPtr<FJsonObject>* NestedPatch = nullptr;
        if (Body->TryGetObjectField(TEXT("patch"), NestedPatch) && NestedPatch && NestedPatch->IsValid())
        {
            Patch = *NestedPatch;
        }

        TSharedPtr<FJsonObject> ResultJson;
        const bool bSave = FParse::Param(*Params, TEXT("Save")) || ABTJson::GetBool(Body, TEXT("saveOnSuccess"), false);
        if (!FABTMaterialTools::PatchMaterial(Patch, bSave, ResultJson, Error))
        {
            UE_LOG(LogTemp, Error, TEXT("%s"), *Error);
            return 1;
        }

        UE_LOG(LogTemp, Display, TEXT("%s"), *ABTJson::ToString(ResultJson));
        return 0;
    }

    if (Mode.Equals(TEXT("performance-analyze"), ESearchCase::IgnoreCase) ||
        Mode.Equals(TEXT("performance-dry-run"), ESearchCase::IgnoreCase) ||
        Mode.Equals(TEXT("performance-apply"), ESearchCase::IgnoreCase))
    {
        TSharedPtr<FJsonObject> Request;
        FString Error;
        if (!LoadRequestFromParam(Params, Request, Error))
        {
            UE_LOG(LogTemp, Error, TEXT("%s"), *Error);
            return 1;
        }

        TSharedPtr<FJsonObject> ResultJson;
        bool bOk = false;
        if (Mode.Equals(TEXT("performance-analyze"), ESearchCase::IgnoreCase))
        {
            bOk = FABTPerformanceTools::AnalyzeProject(Request, ResultJson, Error);
        }
        else if (Mode.Equals(TEXT("performance-dry-run"), ESearchCase::IgnoreCase))
        {
            bOk = FABTPerformanceTools::DryRunOptimization(Request, ResultJson, Error);
        }
        else
        {
            bOk = FABTPerformanceTools::ApplyOptimization(Request, ResultJson, Error);
        }

        if (!bOk)
        {
            UE_LOG(LogTemp, Error, TEXT("%s"), *Error);
            return 1;
        }

        UE_LOG(LogTemp, Display, TEXT("%s"), *ABTJson::ToString(ResultJson));
        return 0;
    }

    FString PatchFile;
    if (!FParse::Value(*Params, TEXT("PatchFile="), PatchFile) || PatchFile.IsEmpty())
    {
        UE_LOG(LogTemp, Error, TEXT("ABTApplyPatch requires -PatchFile=<json path>"));
        return 1;
    }

    FString PatchText;
    if (!FFileHelper::LoadFileToString(PatchText, *PatchFile))
    {
        UE_LOG(LogTemp, Error, TEXT("Could not read patch file: %s"), *PatchFile);
        return 1;
    }

    TSharedPtr<FJsonObject> Body;
    FString Error;
    if (!ABTJson::FromString(PatchText, Body, Error))
    {
        UE_LOG(LogTemp, Error, TEXT("%s"), *Error);
        return 1;
    }

    TSharedPtr<FJsonObject> Patch = Body;
    const TSharedPtr<FJsonObject>* NestedPatch = nullptr;
    if (Body->TryGetObjectField(TEXT("patch"), NestedPatch) && NestedPatch && NestedPatch->IsValid())
    {
        Patch = *NestedPatch;
    }

    TSharedPtr<FJsonObject> DryRunJson;
    if (!FABTBlueprintTools::DryRunPatch(Patch, DryRunJson, Error))
    {
        UE_LOG(LogTemp, Error, TEXT("%s"), *Error);
        return 1;
    }

    if (!DryRunJson->GetBoolField(TEXT("valid")))
    {
        UE_LOG(LogTemp, Error, TEXT("Patch dry-run failed: %s"), *ABTJson::ToString(DryRunJson));
        return 1;
    }

    TSharedPtr<FJsonObject> ResultJson;
    const bool bSave = FParse::Param(*Params, TEXT("Save")) || ABTJson::GetBool(Body, TEXT("saveOnSuccess"), false);
    if (!FABTBlueprintTools::ApplyPatch(Patch, bSave, ResultJson, Error))
    {
        UE_LOG(LogTemp, Error, TEXT("%s"), *Error);
        return 1;
    }

    UE_LOG(LogTemp, Display, TEXT("%s"), *ABTJson::ToString(ResultJson));

    const TSharedPtr<FJsonObject>* CompileJson = nullptr;
    if (ResultJson->TryGetObjectField(TEXT("compile"), CompileJson) && CompileJson && CompileJson->IsValid())
    {
        if (!(*CompileJson)->GetBoolField(TEXT("compile_ok")))
        {
            return 1;
        }
    }

    return 0;
}
