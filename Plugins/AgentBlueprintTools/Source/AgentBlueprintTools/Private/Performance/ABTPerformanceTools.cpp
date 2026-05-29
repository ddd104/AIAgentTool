#include "ABTPerformanceTools.h"
#include "Utils/ABTJson.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"

namespace
{
    struct FABTPerformanceTargets
    {
        bool bAndroid = false;
        bool bWindows = false;
        bool bLinux = false;
        TArray<FString> Names;

        bool HasMobile() const { return bAndroid; }
        bool HasDesktop() const { return bWindows || bLinux; }
    };

    struct FABTPerformanceThresholds
    {
        int32 MaxTextureSize = 2048;
        int32 MaterialExpressionWarning = 120;
        int32 StaticMeshTriangleWarning = 150000;
        bool bAggressive = false;
        bool bAllowVisualChanges = false;
    };

    struct FABTPerformanceStats
    {
        int32 ScannedAssets = 0;
        int32 Textures = 0;
        int32 Materials = 0;
        int32 MaterialInstances = 0;
        int32 StaticMeshes = 0;
        int32 SkeletalMeshes = 0;
        int32 Issues = 0;
        int32 PlannedActions = 0;
        bool bAssetLimitHit = false;
    };

    FString NormalizeTargetName(const FString& InTarget)
    {
        if (InTarget.Equals(TEXT("Android"), ESearchCase::IgnoreCase))
        {
            return TEXT("Android");
        }
        if (InTarget.Equals(TEXT("Win64"), ESearchCase::IgnoreCase) || InTarget.Equals(TEXT("Windows"), ESearchCase::IgnoreCase))
        {
            return TEXT("Windows");
        }
        if (InTarget.Equals(TEXT("Linux"), ESearchCase::IgnoreCase))
        {
            return TEXT("Linux");
        }
        return FString();
    }

    FABTPerformanceTargets ParseTargets(const TSharedPtr<FJsonObject>& Request)
    {
        FABTPerformanceTargets Targets;
        const TArray<TSharedPtr<FJsonValue>>* TargetValues = nullptr;
        if (Request.IsValid() &&
            (Request->TryGetArrayField(TEXT("targetPlatforms"), TargetValues) || Request->TryGetArrayField(TEXT("targets"), TargetValues)) &&
            TargetValues)
        {
            for (const TSharedPtr<FJsonValue>& Value : *TargetValues)
            {
                const FString Target = NormalizeTargetName(Value->AsString());
                if (Target.IsEmpty())
                {
                    continue;
                }
                if (!Targets.Names.Contains(Target))
                {
                    Targets.Names.Add(Target);
                }
            }
        }

        if (Targets.Names.Num() == 0)
        {
            Targets.Names = { TEXT("Android"), TEXT("Windows"), TEXT("Linux") };
        }

        Targets.bAndroid = Targets.Names.Contains(TEXT("Android"));
        Targets.bWindows = Targets.Names.Contains(TEXT("Windows"));
        Targets.bLinux = Targets.Names.Contains(TEXT("Linux"));
        return Targets;
    }

    TArray<FString> ParseContentPaths(const TSharedPtr<FJsonObject>& Request, FString& OutError, bool bUseDefaultPath)
    {
        TArray<FString> Paths;
        const TArray<TSharedPtr<FJsonValue>>* PathValues = nullptr;
        if (Request.IsValid() && Request->TryGetArrayField(TEXT("contentPaths"), PathValues) && PathValues)
        {
            for (const TSharedPtr<FJsonValue>& Value : *PathValues)
            {
                FString Path = Value->AsString();
                Path.TrimStartAndEndInline();
                if (Path.IsEmpty())
                {
                    continue;
                }
                if (!Path.StartsWith(TEXT("/Game")))
                {
                    OutError = FString::Printf(TEXT("contentPaths entries must stay under /Game: %s"), *Path);
                    return {};
                }
                Paths.AddUnique(Path);
            }
        }

        if (Paths.Num() == 0 && bUseDefaultPath)
        {
            Paths.Add(TEXT("/Game"));
        }
        return Paths;
    }

    TArray<FString> ParseAssetPaths(const TSharedPtr<FJsonObject>& Request, FString& OutError)
    {
        TArray<FString> Paths;
        const TArray<TSharedPtr<FJsonValue>>* PathValues = nullptr;
        if (Request.IsValid() && Request->TryGetArrayField(TEXT("assetPaths"), PathValues) && PathValues)
        {
            for (const TSharedPtr<FJsonValue>& Value : *PathValues)
            {
                FString Path = Value->AsString();
                Path.TrimStartAndEndInline();
                if (Path.IsEmpty())
                {
                    continue;
                }
                if (!Path.StartsWith(TEXT("/Game")))
                {
                    OutError = FString::Printf(TEXT("assetPaths entries must stay under /Game: %s"), *Path);
                    return {};
                }
                Paths.AddUnique(Path);
            }
        }
        return Paths;
    }

    FABTPerformanceThresholds GetThresholds(const FABTPerformanceTargets& Targets, const FString& Profile, bool bAllowVisualChanges)
    {
        FABTPerformanceThresholds Thresholds;
        Thresholds.bAggressive =
            Profile.Equals(TEXT("aggressive"), ESearchCase::IgnoreCase) ||
            Profile.Equals(TEXT("mobile"), ESearchCase::IgnoreCase);
        Thresholds.bAllowVisualChanges = bAllowVisualChanges;

        if (Targets.HasMobile())
        {
            Thresholds.MaxTextureSize = Thresholds.bAggressive ? 1024 : 2048;
            Thresholds.MaterialExpressionWarning = Thresholds.bAggressive ? 60 : 80;
            Thresholds.StaticMeshTriangleWarning = Thresholds.bAggressive ? 50000 : 75000;
        }
        else if (Targets.HasDesktop())
        {
            Thresholds.MaxTextureSize = Thresholds.bAggressive ? 2048 : 4096;
            Thresholds.MaterialExpressionWarning = Thresholds.bAggressive ? 120 : 160;
            Thresholds.StaticMeshTriangleWarning = Thresholds.bAggressive ? 150000 : 250000;
        }

        return Thresholds;
    }

    TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        for (const FString& Value : Values)
        {
            Result.Add(ABTJson::StringValue(Value));
        }
        return Result;
    }

    FString BoolString(bool bValue)
    {
        return bValue ? TEXT("true") : TEXT("false");
    }

    FString CompressionToString(TextureCompressionSettings Settings)
    {
        switch (Settings)
        {
            case TC_Default: return TEXT("Default");
            case TC_Normalmap: return TEXT("Normalmap");
            case TC_Masks: return TEXT("Masks");
            case TC_Grayscale: return TEXT("Grayscale");
            case TC_Displacementmap: return TEXT("Displacementmap");
            case TC_VectorDisplacementmap: return TEXT("VectorDisplacementmap");
            case TC_HDR: return TEXT("HDR");
            case TC_EditorIcon: return TEXT("EditorIcon");
            case TC_Alpha: return TEXT("Alpha");
            case TC_DistanceFieldFont: return TEXT("DistanceFieldFont");
            case TC_HDR_Compressed: return TEXT("HDR_Compressed");
            case TC_BC7: return TEXT("BC7");
            case TC_HalfFloat: return TEXT("HalfFloat");
            case TC_LQ: return TEXT("LQ");
            case TC_SingleFloat: return TEXT("SingleFloat");
            case TC_HDR_F32: return TEXT("HDR_F32");
            default: return FString::FromInt(static_cast<int32>(Settings));
        }
    }

    FString MipGenToString(TextureMipGenSettings Settings)
    {
        return UTexture::GetMipGenSettingsString(Settings);
    }

    FString PerformanceBlendModeToString(EBlendMode BlendMode)
    {
        switch (BlendMode)
        {
            case BLEND_Opaque: return TEXT("Opaque");
            case BLEND_Masked: return TEXT("Masked");
            case BLEND_Translucent: return TEXT("Translucent");
            case BLEND_Additive: return TEXT("Additive");
            case BLEND_Modulate: return TEXT("Modulate");
            case BLEND_AlphaComposite: return TEXT("AlphaComposite");
            case BLEND_AlphaHoldout: return TEXT("AlphaHoldout");
            default: return TEXT("Unknown");
        }
    }

    bool IsTextureGroupUiLike(TextureGroup Group)
    {
        return Group == TEXTUREGROUP_UI ||
            Group == TEXTUREGROUP_Pixels2D ||
            Group == TEXTUREGROUP_ColorLookupTable ||
            Group == TEXTUREGROUP_IESLightProfile;
    }

    bool NameLooksLikeMaskTexture(const FString& Name)
    {
        return Name.Contains(TEXT("_M"), ESearchCase::IgnoreCase) ||
            Name.Contains(TEXT("_ORM"), ESearchCase::IgnoreCase) ||
            Name.Contains(TEXT("_RMA"), ESearchCase::IgnoreCase) ||
            Name.Contains(TEXT("Mask"), ESearchCase::IgnoreCase) ||
            Name.Contains(TEXT("Roughness"), ESearchCase::IgnoreCase) ||
            Name.Contains(TEXT("Metallic"), ESearchCase::IgnoreCase) ||
            Name.Contains(TEXT("Occlusion"), ESearchCase::IgnoreCase);
    }

    TSharedPtr<FJsonObject> MakeIssue(
        const FString& Severity,
        const FString& Category,
        const FString& AssetPath,
        const FString& Message,
        const FString& Recommendation,
        const TArray<FString>& Targets)
    {
        TSharedPtr<FJsonObject> Issue = ABTJson::Object();
        Issue->SetStringField(TEXT("severity"), Severity);
        Issue->SetStringField(TEXT("category"), Category);
        if (!AssetPath.IsEmpty())
        {
            Issue->SetStringField(TEXT("asset_path"), AssetPath);
        }
        Issue->SetStringField(TEXT("message"), Message);
        Issue->SetStringField(TEXT("recommendation"), Recommendation);
        Issue->SetArrayField(TEXT("targets"), StringArrayToJson(Targets));
        return Issue;
    }

    TSharedPtr<FJsonObject> MakeAssetAction(
        const FString& Op,
        const FString& Category,
        const FString& AssetPath,
        const FString& Property,
        const FString& From,
        const FString& To,
        const FString& Reason,
        const FString& Risk,
        bool bAutoApply,
        bool bRequiresVisualReview,
        const TArray<FString>& Targets)
    {
        TSharedPtr<FJsonObject> Action = ABTJson::Object();
        Action->SetStringField(TEXT("op"), Op);
        Action->SetStringField(TEXT("category"), Category);
        Action->SetStringField(TEXT("asset_path"), AssetPath);
        Action->SetStringField(TEXT("property"), Property);
        Action->SetStringField(TEXT("from"), From);
        Action->SetStringField(TEXT("to"), To);
        Action->SetStringField(TEXT("reason"), Reason);
        Action->SetStringField(TEXT("risk"), Risk);
        Action->SetBoolField(TEXT("auto_apply"), bAutoApply);
        Action->SetBoolField(TEXT("requires_visual_review"), bRequiresVisualReview);
        Action->SetArrayField(TEXT("targets"), StringArrayToJson(Targets));
        return Action;
    }

    FString ConfigFilePath(const FString& FileName)
    {
        return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), FileName));
    }

    FString GetConfigValue(const FString& Filename, const FString& Section, const FString& Key)
    {
        FConfigFile ConfigFile;
        if (FPaths::FileExists(Filename))
        {
            ConfigFile.Read(Filename);
        }

        const FConfigSection* ConfigSection = ConfigFile.FindSection(Section);
        if (!ConfigSection)
        {
            return FString();
        }

        const FConfigValue* Value = ConfigSection->Find(FName(*Key));
        return Value ? Value->GetValue() : FString();
    }

    bool WriteConfigValue(const FString& Filename, const FString& Section, const FString& Key, const FString& Value, FString& OutError)
    {
        FConfigFile ConfigFile;
        if (FPaths::FileExists(Filename))
        {
            ConfigFile.Read(Filename);
        }

        ConfigFile.SetString(*Section, *Key, *Value);
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
        if (!ConfigFile.Write(Filename))
        {
            OutError = FString::Printf(TEXT("Failed to write config file: %s"), *Filename);
            return false;
        }

        if (GConfig)
        {
            GConfig->SetString(*Section, *Key, *Value, Filename);
            GConfig->Flush(false, Filename);
        }
        return true;
    }

    bool TryParseSectionHeader(const FString& Line, FString& OutSection)
    {
        FString Trimmed = Line;
        Trimmed.TrimStartAndEndInline();
        if (Trimmed.Len() < 3 || Trimmed[0] != TCHAR('[') || Trimmed[Trimmed.Len() - 1] != TCHAR(']'))
        {
            return false;
        }

        OutSection = Trimmed.Mid(1, Trimmed.Len() - 2);
        OutSection.TrimStartAndEndInline();
        return !OutSection.IsEmpty();
    }

    bool TryParseCVarLine(const FString& Line, FString& OutName, FString& OutValue)
    {
        FString Trimmed = Line;
        Trimmed.TrimStartAndEndInline();
        if (!Trimmed.StartsWith(TEXT("+CVars=")) && !Trimmed.StartsWith(TEXT("CVars=")))
        {
            return false;
        }

        int32 PrefixEquals = INDEX_NONE;
        if (!Trimmed.FindChar(TCHAR('='), PrefixEquals))
        {
            return false;
        }

        FString Assignment = Trimmed.Mid(PrefixEquals + 1);
        Assignment.TrimStartAndEndInline();
        Assignment.TrimQuotesInline();

        int32 CVarEquals = INDEX_NONE;
        if (!Assignment.FindChar(TCHAR('='), CVarEquals))
        {
            OutName = Assignment;
            OutValue.Reset();
        }
        else
        {
            OutName = Assignment.Left(CVarEquals);
            OutValue = Assignment.Mid(CVarEquals + 1);
        }

        OutName.TrimStartAndEndInline();
        OutValue.TrimStartAndEndInline();
        return !OutName.IsEmpty();
    }

    FString GetDeviceProfileCVarValue(const FString& Filename, const FString& Section, const FString& CVar)
    {
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *Filename))
        {
            return FString();
        }

        TArray<FString> Lines;
        Text.ParseIntoArrayLines(Lines, false);
        bool bInTargetSection = false;

        for (const FString& Line : Lines)
        {
            FString CurrentSection;
            if (TryParseSectionHeader(Line, CurrentSection))
            {
                bInTargetSection = CurrentSection.Equals(Section, ESearchCase::IgnoreCase);
                continue;
            }

            if (!bInTargetSection)
            {
                continue;
            }

            FString Name;
            FString Value;
            if (TryParseCVarLine(Line, Name, Value) && Name.Equals(CVar, ESearchCase::IgnoreCase))
            {
                return Value;
            }
        }

        return FString();
    }

    bool WriteDeviceProfileCVarValue(
        const FString& Filename,
        const FString& Section,
        const FString& CVar,
        const FString& Value,
        FString& OutError)
    {
        FString Text;
        if (FPaths::FileExists(Filename) && !FFileHelper::LoadFileToString(Text, *Filename))
        {
            OutError = FString::Printf(TEXT("Failed to read config file: %s"), *Filename);
            return false;
        }

        TArray<FString> Lines;
        Text.ParseIntoArrayLines(Lines, false);

        const FString NewLine = FString::Printf(TEXT("+CVars=%s=%s"), *CVar, *Value);
        bool bFoundSection = false;
        bool bInTargetSection = false;
        int32 InsertIndex = Lines.Num();

        for (int32 Index = 0; Index < Lines.Num(); ++Index)
        {
            FString CurrentSection;
            if (TryParseSectionHeader(Lines[Index], CurrentSection))
            {
                if (bInTargetSection)
                {
                    InsertIndex = Index;
                    bInTargetSection = false;
                }

                if (CurrentSection.Equals(Section, ESearchCase::IgnoreCase))
                {
                    bFoundSection = true;
                    bInTargetSection = true;
                    InsertIndex = Index + 1;
                }
                continue;
            }

            if (!bInTargetSection)
            {
                continue;
            }

            InsertIndex = Index + 1;

            FString Name;
            FString ExistingValue;
            if (TryParseCVarLine(Lines[Index], Name, ExistingValue) && Name.Equals(CVar, ESearchCase::IgnoreCase))
            {
                Lines[Index] = NewLine;
                IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
                const FString Output = FString::Join(Lines, LINE_TERMINATOR) + LINE_TERMINATOR;
                if (!FFileHelper::SaveStringToFile(Output, *Filename))
                {
                    OutError = FString::Printf(TEXT("Failed to write config file: %s"), *Filename);
                    return false;
                }
                return true;
            }
        }

        if (!bFoundSection)
        {
            if (Lines.Num() > 0 && !Lines.Last().IsEmpty())
            {
                Lines.Add(FString());
            }
            Lines.Add(FString::Printf(TEXT("[%s]"), *Section));
            Lines.Add(NewLine);
        }
        else
        {
            Lines.Insert(NewLine, InsertIndex);
        }

        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
        const FString Output = FString::Join(Lines, LINE_TERMINATOR) + LINE_TERMINATOR;
        if (!FFileHelper::SaveStringToFile(Output, *Filename))
        {
            OutError = FString::Printf(TEXT("Failed to write config file: %s"), *Filename);
            return false;
        }
        return true;
    }

    TSharedPtr<FJsonObject> MakeConfigAction(
        const FString& File,
        const FString& Section,
        const FString& Key,
        const FString& To,
        const FString& Reason,
        const FString& Risk,
        const TArray<FString>& Targets,
        bool bAutoApply = true,
        bool bRequiresVisualReview = false)
    {
        TSharedPtr<FJsonObject> Action = ABTJson::Object();
        Action->SetStringField(TEXT("op"), TEXT("set_config_value"));
        Action->SetStringField(TEXT("category"), TEXT("project_setting"));
        Action->SetStringField(TEXT("file"), File);
        Action->SetStringField(TEXT("section"), Section);
        Action->SetStringField(TEXT("key"), Key);
        Action->SetStringField(TEXT("from"), GetConfigValue(File, Section, Key));
        Action->SetStringField(TEXT("to"), To);
        Action->SetStringField(TEXT("reason"), Reason);
        Action->SetStringField(TEXT("risk"), Risk);
        Action->SetBoolField(TEXT("auto_apply"), bAutoApply);
        Action->SetBoolField(TEXT("requires_visual_review"), bRequiresVisualReview);
        Action->SetArrayField(TEXT("targets"), StringArrayToJson(Targets));
        return Action;
    }

    TSharedPtr<FJsonObject> MakeDeviceProfileCVarAction(
        const FString& File,
        const FString& Section,
        const FString& CVar,
        const FString& To,
        const FString& Reason,
        const FString& Risk,
        const TArray<FString>& Targets,
        bool bAutoApply = true,
        bool bRequiresVisualReview = false)
    {
        TSharedPtr<FJsonObject> Action = ABTJson::Object();
        Action->SetStringField(TEXT("op"), TEXT("set_device_profile_cvar"));
        Action->SetStringField(TEXT("category"), TEXT("project_setting"));
        Action->SetStringField(TEXT("file"), File);
        Action->SetStringField(TEXT("section"), Section);
        Action->SetStringField(TEXT("key"), TEXT("+CVars"));
        Action->SetStringField(TEXT("cvar"), CVar);
        Action->SetStringField(TEXT("from"), GetDeviceProfileCVarValue(File, Section, CVar));
        Action->SetStringField(TEXT("to"), To);
        Action->SetStringField(TEXT("reason"), Reason);
        Action->SetStringField(TEXT("risk"), Risk);
        Action->SetBoolField(TEXT("auto_apply"), bAutoApply);
        Action->SetBoolField(TEXT("requires_visual_review"), bRequiresVisualReview);
        Action->SetArrayField(TEXT("targets"), StringArrayToJson(Targets));
        return Action;
    }

    void AddConfigActionIfChanged(
        TArray<TSharedPtr<FJsonValue>>& Plan,
        TSet<FString>& SeenKeys,
        const FString& File,
        const FString& Section,
        const FString& Key,
        const FString& To,
        const FString& Reason,
        const FString& Risk,
        const TArray<FString>& Targets,
        bool bAutoApply = true,
        bool bRequiresVisualReview = false)
    {
        const FString DedupKey = File + TEXT("|") + Section + TEXT("|") + Key;
        if (SeenKeys.Contains(DedupKey))
        {
            return;
        }
        SeenKeys.Add(DedupKey);

        const FString Current = GetConfigValue(File, Section, Key);
        if (Current.Equals(To, ESearchCase::IgnoreCase))
        {
            return;
        }

        Plan.Add(ABTJson::ObjectValue(MakeConfigAction(File, Section, Key, To, Reason, Risk, Targets, bAutoApply, bRequiresVisualReview)));
    }

    void AddDeviceProfileCVarActionIfChanged(
        TArray<TSharedPtr<FJsonValue>>& Plan,
        TSet<FString>& SeenKeys,
        const FString& File,
        const FString& Section,
        const FString& CVar,
        const FString& To,
        const FString& Reason,
        const FString& Risk,
        const TArray<FString>& Targets,
        bool bAutoApply = true,
        bool bRequiresVisualReview = false)
    {
        const FString DedupKey = File + TEXT("|") + Section + TEXT("|+CVars|") + CVar;
        if (SeenKeys.Contains(DedupKey))
        {
            return;
        }
        SeenKeys.Add(DedupKey);

        const FString Current = GetDeviceProfileCVarValue(File, Section, CVar);
        if (Current.Equals(To, ESearchCase::IgnoreCase))
        {
            return;
        }

        Plan.Add(ABTJson::ObjectValue(MakeDeviceProfileCVarAction(File, Section, CVar, To, Reason, Risk, Targets, bAutoApply, bRequiresVisualReview)));
    }

    void AddProjectSettingActions(
        const FABTPerformanceTargets& Targets,
        const FString& Profile,
        TArray<TSharedPtr<FJsonValue>>& Plan)
    {
        const FString EngineFile = ConfigFilePath(TEXT("DefaultEngine.ini"));
        TSet<FString> SeenKeys;
        const TArray<FString>& AllTargets = Targets.Names;
        const bool bAggressive = Profile.Equals(TEXT("aggressive"), ESearchCase::IgnoreCase) || Profile.Equals(TEXT("mobile"), ESearchCase::IgnoreCase);

        AddConfigActionIfChanged(
            Plan,
            SeenKeys,
            EngineFile,
            TEXT("/Script/Engine.RendererSettings"),
            TEXT("r.DefaultFeature.MotionBlur"),
            TEXT("False"),
            TEXT("Disable default motion blur for cheaper post processing."),
            TEXT("low"),
            AllTargets);

        if (bAggressive)
        {
            AddConfigActionIfChanged(
                Plan,
                SeenKeys,
                EngineFile,
                TEXT("/Script/Engine.RendererSettings"),
                TEXT("r.DefaultFeature.Bloom"),
                TEXT("False"),
                TEXT("Disable default bloom for aggressive performance profile."),
                TEXT("medium"),
                AllTargets);

            AddConfigActionIfChanged(
                Plan,
                SeenKeys,
                EngineFile,
                TEXT("/Script/Engine.RendererSettings"),
                TEXT("r.DefaultFeature.AmbientOcclusion"),
                TEXT("False"),
                TEXT("Disable default ambient occlusion for aggressive performance profile."),
                TEXT("medium"),
                AllTargets);
        }

        if (Targets.bAndroid)
        {
            const TArray<FString> AndroidTargets = { TEXT("Android") };
            const FString DeviceProfileFile = ConfigFilePath(TEXT("DefaultDeviceProfiles.ini"));
            const FString AndroidDeviceProfileSection = TEXT("Android DeviceProfile");

            AddConfigActionIfChanged(Plan, SeenKeys, EngineFile, TEXT("/Script/Engine.RendererSettings"), TEXT("r.TextureStreaming"), TEXT("True"), TEXT("Enable texture streaming so Android GPU memory is managed by the streamer."), TEXT("medium"), AndroidTargets);
            AddConfigActionIfChanged(Plan, SeenKeys, EngineFile, TEXT("/Script/Engine.RendererSettings"), TEXT("r.ReflectionCaptureResolution"), TEXT("512"), TEXT("Reduce reflection capture cubemap size for lower Android GPU memory and sampling cost."), TEXT("medium"), AndroidTargets);
            AddConfigActionIfChanged(Plan, SeenKeys, EngineFile, TEXT("/Script/Engine.RendererSettings"), TEXT("r.Mobile.FSR.Enabled"), TEXT("1"), TEXT("Enable mobile FSR so Android can recover image quality when rendering below native resolution."), TEXT("low"), AndroidTargets);
            AddConfigActionIfChanged(Plan, SeenKeys, EngineFile, TEXT("/Script/Engine.RendererSettings"), TEXT("r.MobileHDR"), TEXT("False"), TEXT("Disable mobile HDR for Android to reduce bandwidth and post cost."), TEXT("high"), AndroidTargets, true, true);
            AddConfigActionIfChanged(Plan, SeenKeys, EngineFile, TEXT("/Script/Engine.RendererSettings"), TEXT("r.GenerateMeshDistanceFields"), TEXT("False"), TEXT("Disable mesh distance fields for Android performance profile."), TEXT("medium"), AndroidTargets);
            AddConfigActionIfChanged(Plan, SeenKeys, EngineFile, TEXT("/Script/Engine.RendererSettings"), TEXT("r.DynamicGlobalIlluminationMethod"), TEXT("0"), TEXT("Disable heavyweight dynamic GI for Android profile."), TEXT("medium"), AndroidTargets);
            AddConfigActionIfChanged(Plan, SeenKeys, EngineFile, TEXT("/Script/Engine.RendererSettings"), TEXT("r.ReflectionMethod"), TEXT("0"), TEXT("Disable heavyweight dynamic reflections for Android profile."), TEXT("high"), AndroidTargets, true, true);
            AddConfigActionIfChanged(Plan, SeenKeys, EngineFile, TEXT("/Script/Engine.RendererSettings"), TEXT("r.Shadow.Virtual.Enable"), TEXT("0"), TEXT("Disable virtual shadow maps for Android profile."), TEXT("medium"), AndroidTargets);
            AddConfigActionIfChanged(Plan, SeenKeys, EngineFile, TEXT("/Script/HardwareTargeting.HardwareTargetingSettings"), TEXT("TargetedHardwareClass"), TEXT("Mobile"), TEXT("Set project hardware targeting toward mobile when optimizing for Android."), TEXT("medium"), AndroidTargets);
            AddConfigActionIfChanged(Plan, SeenKeys, EngineFile, TEXT("/Script/HardwareTargeting.HardwareTargetingSettings"), TEXT("DefaultGraphicsPerformance"), TEXT("Scalable"), TEXT("Use scalable defaults for Android-targeted builds."), TEXT("medium"), AndroidTargets);

            AddDeviceProfileCVarActionIfChanged(Plan, SeenKeys, DeviceProfileFile, AndroidDeviceProfileSection, TEXT("r.Mobile.FSR.Enabled"), TEXT("1"), TEXT("Keep Android runtime aligned with project-level mobile FSR."), TEXT("low"), AndroidTargets);
            AddDeviceProfileCVarActionIfChanged(Plan, SeenKeys, DeviceProfileFile, AndroidDeviceProfileSection, TEXT("r.ScreenPercentage"), TEXT("75"), TEXT("Render Android Vulkan at a controlled internal resolution and upscale with mobile FSR."), TEXT("medium"), AndroidTargets);
            AddDeviceProfileCVarActionIfChanged(Plan, SeenKeys, DeviceProfileFile, AndroidDeviceProfileSection, TEXT("r.Tonemapper.Quality"), TEXT("0"), TEXT("Use the cheapest tonemapper path for Android mobile rendering."), TEXT("medium"), AndroidTargets);
            AddDeviceProfileCVarActionIfChanged(Plan, SeenKeys, DeviceProfileFile, AndroidDeviceProfileSection, TEXT("r.EyeAdaptationQuality"), TEXT("0"), TEXT("Disable eye adaptation passes on Android."), TEXT("low"), AndroidTargets);
            AddDeviceProfileCVarActionIfChanged(Plan, SeenKeys, DeviceProfileFile, AndroidDeviceProfileSection, TEXT("r.MotionBlurQuality"), TEXT("0"), TEXT("Disable motion blur passes on Android."), TEXT("low"), AndroidTargets);
            AddDeviceProfileCVarActionIfChanged(Plan, SeenKeys, DeviceProfileFile, AndroidDeviceProfileSection, TEXT("r.AmbientOcclusionLevels"), TEXT("0"), TEXT("Disable ambient occlusion passes on Android."), TEXT("low"), AndroidTargets);
            AddDeviceProfileCVarActionIfChanged(Plan, SeenKeys, DeviceProfileFile, AndroidDeviceProfileSection, TEXT("r.SceneColorFringeQuality"), TEXT("0"), TEXT("Disable chromatic aberration on Android."), TEXT("low"), AndroidTargets);
            AddDeviceProfileCVarActionIfChanged(Plan, SeenKeys, DeviceProfileFile, AndroidDeviceProfileSection, TEXT("r.TranslucencyVolumeBlur"), TEXT("0"), TEXT("Disable translucency volume blur on Android."), TEXT("low"), AndroidTargets);

            if (bAggressive)
            {
                AddDeviceProfileCVarActionIfChanged(Plan, SeenKeys, DeviceProfileFile, AndroidDeviceProfileSection, TEXT("r.Vulkan.RobustBufferAccess"), TEXT("0"), TEXT("Disable Vulkan robust buffer access in mobile/aggressive profiles to reduce driver-side bounds checking cost."), TEXT("medium"), AndroidTargets);
            }
        }

        if (Targets.HasDesktop() && bAggressive)
        {
            TArray<FString> DesktopTargets;
            if (Targets.bWindows) DesktopTargets.Add(TEXT("Windows"));
            if (Targets.bLinux) DesktopTargets.Add(TEXT("Linux"));
            AddConfigActionIfChanged(Plan, SeenKeys, EngineFile, TEXT("/Script/Engine.RendererSettings"), TEXT("r.DynamicGlobalIlluminationMethod"), TEXT("0"), TEXT("Disable heavyweight dynamic GI for aggressive desktop profile."), TEXT("medium"), DesktopTargets);
            AddConfigActionIfChanged(Plan, SeenKeys, EngineFile, TEXT("/Script/Engine.RendererSettings"), TEXT("r.ReflectionMethod"), TEXT("0"), TEXT("Disable heavyweight dynamic reflections for aggressive desktop profile."), TEXT("medium"), DesktopTargets);
            AddConfigActionIfChanged(Plan, SeenKeys, EngineFile, TEXT("/Script/Engine.RendererSettings"), TEXT("r.Shadow.Virtual.Enable"), TEXT("0"), TEXT("Disable virtual shadow maps for aggressive desktop profile."), TEXT("medium"), DesktopTargets);
            AddConfigActionIfChanged(Plan, SeenKeys, EngineFile, TEXT("/Script/Engine.RendererSettings"), TEXT("r.RayTracing.RayTracingProxies.ProjectEnabled"), TEXT("False"), TEXT("Disable ray tracing proxy generation for aggressive desktop profile."), TEXT("medium"), DesktopTargets);
        }
    }

    bool CollectAssets(
        const TArray<FString>& ContentPaths,
        int32 MaxAssets,
        TArray<FAssetData>& OutAssets,
        FString& OutError)
    {
        if (MaxAssets <= 0)
        {
            OutError = TEXT("maxAssets must be greater than 0");
            return false;
        }

        FARFilter Filter;
        Filter.bRecursivePaths = true;
        Filter.bRecursiveClasses = true;
        Filter.ClassPaths.Add(UTexture2D::StaticClass()->GetClassPathName());
        Filter.ClassPaths.Add(UMaterial::StaticClass()->GetClassPathName());
        Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());
        Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
        Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());

        for (const FString& Path : ContentPaths)
        {
            Filter.PackagePaths.Add(*Path);
        }

        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        AssetRegistryModule.Get().GetAssets(Filter, OutAssets);
        if (OutAssets.Num() > MaxAssets)
        {
            OutAssets.SetNum(MaxAssets);
        }
        return true;
    }

    bool IsSupportedPerformanceAsset(UObject* Asset)
    {
        return Cast<UTexture2D>(Asset) ||
            Cast<UMaterial>(Asset) ||
            Cast<UMaterialInstanceConstant>(Asset) ||
            Cast<UStaticMesh>(Asset) ||
            Cast<USkeletalMesh>(Asset);
    }

    bool CollectExactAssets(
        const TArray<FString>& AssetPaths,
        int32 MaxAssets,
        TArray<FAssetData>& OutAssets,
        FString& OutError)
    {
        for (const FString& AssetPath : AssetPaths)
        {
            if (OutAssets.Num() >= MaxAssets)
            {
                break;
            }

            UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
            if (!Asset && !AssetPath.Contains(TEXT(".")))
            {
                const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
                Asset = LoadObject<UObject>(nullptr, *ObjectPath);
            }

            if (!Asset)
            {
                OutError = FString::Printf(TEXT("Could not load assetPath: %s"), *AssetPath);
                return false;
            }

            if (!IsSupportedPerformanceAsset(Asset))
            {
                OutError = FString::Printf(TEXT("Unsupported performance asset type for %s: %s"), *AssetPath, *Asset->GetClass()->GetPathName());
                return false;
            }

            OutAssets.Add(FAssetData(Asset));
        }
        return true;
    }

    void DeduplicateAssets(TArray<FAssetData>& Assets)
    {
        TSet<FString> Seen;
        TArray<FAssetData> Unique;
        for (const FAssetData& Asset : Assets)
        {
            const FString Key = Asset.GetObjectPathString();
            if (Key.IsEmpty() || Seen.Contains(Key))
            {
                continue;
            }
            Seen.Add(Key);
            Unique.Add(Asset);
        }
        Assets = MoveTemp(Unique);
    }

    void AnalyzeTexture(
        UTexture2D* Texture,
        const FABTPerformanceTargets& Targets,
        const FABTPerformanceThresholds& Thresholds,
        TArray<TSharedPtr<FJsonValue>>& Issues,
        TArray<TSharedPtr<FJsonValue>>* Plan,
        FABTPerformanceStats& Stats)
    {
        if (!Texture)
        {
            return;
        }

        ++Stats.Textures;
        const FString AssetPath = Texture->GetPathName();
        const int32 Width = Texture->GetSizeX();
        const int32 Height = Texture->GetSizeY();
        const int32 LargestDimension = FMath::Max(Width, Height);
        const bool bUiLike = IsTextureGroupUiLike(Texture->LODGroup);

        if (LargestDimension > Thresholds.MaxTextureSize)
        {
            ++Stats.Issues;
            Issues.Add(ABTJson::ObjectValue(MakeIssue(
                TEXT("warning"),
                TEXT("texture"),
                AssetPath,
                FString::Printf(TEXT("Texture is %dx%d, above target max %d."), Width, Height, Thresholds.MaxTextureSize),
                TEXT("Set Maximum Texture Size or split platform-specific texture groups."),
                Targets.Names)));

            if (Plan)
            {
                ++Stats.PlannedActions;
                Plan->Add(ABTJson::ObjectValue(MakeAssetAction(
                    TEXT("set_texture_max_size"),
                    TEXT("texture"),
                    AssetPath,
                    TEXT("MaxTextureSize"),
                    FString::FromInt(Texture->MaxTextureSize),
                    FString::FromInt(Thresholds.MaxTextureSize),
                    TEXT("Clamp large texture resolution for selected target platforms."),
                    TEXT("low"),
                    true,
                    false,
                    Targets.Names)));
            }
        }

        if (!bUiLike && Texture->MipGenSettings == TMGS_NoMipmaps)
        {
            ++Stats.Issues;
            Issues.Add(ABTJson::ObjectValue(MakeIssue(
                TEXT("warning"),
                TEXT("texture"),
                AssetPath,
                TEXT("Texture has mip generation disabled."),
                TEXT("Use mipmaps for world/material textures to reduce aliasing and texture cache pressure."),
                Targets.Names)));

            if (Plan)
            {
                ++Stats.PlannedActions;
                Plan->Add(ABTJson::ObjectValue(MakeAssetAction(
                    TEXT("set_texture_mips_from_group"),
                    TEXT("texture"),
                    AssetPath,
                    TEXT("MipGenSettings"),
                    MipGenToString(Texture->MipGenSettings),
                    TEXT("FromTextureGroup"),
                    TEXT("Enable group-driven mip generation for non-UI textures."),
                    TEXT("low"),
                    true,
                    false,
                    Targets.Names)));
            }
        }

        if (!bUiLike && Texture->NeverStream)
        {
            ++Stats.Issues;
            Issues.Add(ABTJson::ObjectValue(MakeIssue(
                TEXT("info"),
                TEXT("texture"),
                AssetPath,
                TEXT("Texture is marked NeverStream."),
                TEXT("Allow streaming for large world textures unless they are always resident by design."),
                Targets.Names)));

            if (Plan)
            {
                ++Stats.PlannedActions;
                Plan->Add(ABTJson::ObjectValue(MakeAssetAction(
                    TEXT("allow_texture_streaming"),
                    TEXT("texture"),
                    AssetPath,
                    TEXT("NeverStream"),
                    BoolString(Texture->NeverStream),
                    TEXT("false"),
                    TEXT("Allow the texture streamer to manage residency."),
                    TEXT("low"),
                    true,
                    false,
                    Targets.Names)));
            }
        }

        if (Targets.bAndroid && Texture->VirtualTextureStreaming)
        {
            ++Stats.Issues;
            Issues.Add(ABTJson::ObjectValue(MakeIssue(
                TEXT("info"),
                TEXT("texture"),
                AssetPath,
                TEXT("Virtual texture streaming is enabled on an Android-targeted optimization pass."),
                TEXT("Verify mobile VT support and memory behavior; disable on textures that do not require VT."),
                { TEXT("Android") })));

            if (Plan)
            {
                ++Stats.PlannedActions;
                Plan->Add(ABTJson::ObjectValue(MakeAssetAction(
                    TEXT("disable_texture_virtual_streaming"),
                    TEXT("texture"),
                    AssetPath,
                    TEXT("VirtualTextureStreaming"),
                    BoolString(Texture->VirtualTextureStreaming),
                    TEXT("false"),
                    TEXT("Use regular streaming for Android unless VT is intentionally required."),
                    TEXT("medium"),
                    true,
                    false,
                    { TEXT("Android") })));
            }
        }

        if (NameLooksLikeMaskTexture(Texture->GetName()) && Texture->SRGB)
        {
            ++Stats.Issues;
            Issues.Add(ABTJson::ObjectValue(MakeIssue(
                TEXT("info"),
                TEXT("texture"),
                AssetPath,
                TEXT("Texture name looks like packed mask data but sRGB is enabled."),
                TEXT("Packed roughness/metallic/AO masks usually use linear color and Masks compression."),
                Targets.Names)));

            if (Plan)
            {
                ++Stats.PlannedActions;
                Plan->Add(ABTJson::ObjectValue(MakeAssetAction(
                    TEXT("set_mask_texture_settings"),
                    TEXT("texture"),
                    AssetPath,
                    TEXT("CompressionSettings/SRGB"),
                    CompressionToString(Texture->CompressionSettings) + TEXT("/") + BoolString(Texture->SRGB),
                    TEXT("Masks/false"),
                    TEXT("Use linear mask compression for heuristic mask textures."),
                    TEXT("medium"),
                    true,
                    true,
                    Targets.Names)));
            }
        }
    }

    void AnalyzeMaterial(
        UMaterial* Material,
        const FABTPerformanceTargets& Targets,
        const FABTPerformanceThresholds& Thresholds,
        TArray<TSharedPtr<FJsonValue>>& Issues,
        TArray<TSharedPtr<FJsonValue>>* Plan,
        FABTPerformanceStats& Stats)
    {
        if (!Material)
        {
            return;
        }

        ++Stats.Materials;
        const FString AssetPath = Material->GetPathName();
        int32 TextureSampleCount = 0;
        int32 CustomExpressionCount = 0;
        for (UMaterialExpression* Expression : Material->GetExpressions())
        {
            if (Cast<UMaterialExpressionTextureSample>(Expression))
            {
                ++TextureSampleCount;
            }
            if (Cast<UMaterialExpressionCustom>(Expression))
            {
                ++CustomExpressionCount;
            }
        }

        const int32 ExpressionCount = Material->GetExpressions().Num();
        if (ExpressionCount > Thresholds.MaterialExpressionWarning)
        {
            ++Stats.Issues;
            Issues.Add(ABTJson::ObjectValue(MakeIssue(
                TEXT("warning"),
                TEXT("material"),
                AssetPath,
                FString::Printf(TEXT("Material has %d expressions and %d texture samples."), ExpressionCount, TextureSampleCount),
                TEXT("Split features with static switches, simplify layers, or use platform-specific material instances."),
                Targets.Names)));
        }

        if (Targets.bAndroid && Material->BlendMode != BLEND_Opaque && Material->BlendMode != BLEND_Masked)
        {
            ++Stats.Issues;
            Issues.Add(ABTJson::ObjectValue(MakeIssue(
                TEXT("warning"),
                TEXT("material"),
                AssetPath,
                FString::Printf(TEXT("Android target uses expensive blend mode: %s."), *PerformanceBlendModeToString(Material->BlendMode)),
                TEXT("Prefer Opaque or Masked materials on mobile where possible."),
                { TEXT("Android") })));
        }

        if (Material->TwoSided)
        {
            ++Stats.Issues;
            Issues.Add(ABTJson::ObjectValue(MakeIssue(
                TEXT("info"),
                TEXT("material"),
                AssetPath,
                TEXT("Material is Two Sided."),
                TEXT("Disable Two Sided unless the asset truly needs backface rendering."),
                Targets.Names)));

            if (Plan && Thresholds.bAllowVisualChanges)
            {
                ++Stats.PlannedActions;
                Plan->Add(ABTJson::ObjectValue(MakeAssetAction(
                    TEXT("disable_material_two_sided"),
                    TEXT("material"),
                    AssetPath,
                    TEXT("TwoSided"),
                    BoolString(Material->TwoSided),
                    TEXT("false"),
                    TEXT("Reduce material raster cost by disabling two-sided rendering."),
                    TEXT("high"),
                    true,
                    true,
                    Targets.Names)));
            }
        }

        if (Targets.bAndroid && !Material->bFullyRough && Thresholds.bAllowVisualChanges)
        {
            if (Plan)
            {
                ++Stats.PlannedActions;
                Plan->Add(ABTJson::ObjectValue(MakeAssetAction(
                    TEXT("set_material_fully_rough"),
                    TEXT("material"),
                    AssetPath,
                    TEXT("bFullyRough"),
                    BoolString(Material->bFullyRough),
                    TEXT("true"),
                    TEXT("Fully rough mobile materials save shader instructions and one sampler."),
                    TEXT("high"),
                    true,
                    true,
                    { TEXT("Android") })));
            }
        }

        if (CustomExpressionCount > 0)
        {
            ++Stats.Issues;
            Issues.Add(ABTJson::ObjectValue(MakeIssue(
                TEXT("info"),
                TEXT("material"),
                AssetPath,
                FString::Printf(TEXT("Material contains %d Custom expression(s)."), CustomExpressionCount),
                TEXT("Review custom HLSL for mobile compatibility and shader instruction count."),
                Targets.Names)));
        }
    }

    void AnalyzeMaterialInstance(
        UMaterialInstanceConstant* MaterialInstance,
        TArray<TSharedPtr<FJsonValue>>& Issues,
        FABTPerformanceStats& Stats,
        const FABTPerformanceTargets& Targets)
    {
        if (!MaterialInstance)
        {
            return;
        }

        ++Stats.MaterialInstances;
        if (!MaterialInstance->Parent)
        {
            ++Stats.Issues;
            Issues.Add(ABTJson::ObjectValue(MakeIssue(
                TEXT("warning"),
                TEXT("material_instance"),
                MaterialInstance->GetPathName(),
                TEXT("Material instance has no parent."),
                TEXT("Assign a parent material to avoid fallback/default shader behavior."),
                Targets.Names)));
        }
    }

    void AnalyzeStaticMesh(
        UStaticMesh* Mesh,
        const FABTPerformanceTargets& Targets,
        const FABTPerformanceThresholds& Thresholds,
        TArray<TSharedPtr<FJsonValue>>& Issues,
        FABTPerformanceStats& Stats)
    {
        if (!Mesh)
        {
            return;
        }

        ++Stats.StaticMeshes;
        const int32 LODCount = Mesh->GetNumLODs();
        const int32 TrianglesLod0 = LODCount > 0 ? Mesh->GetNumTriangles(0) : 0;
        const FString AssetPath = Mesh->GetPathName();

        if (TrianglesLod0 > Thresholds.StaticMeshTriangleWarning)
        {
            ++Stats.Issues;
            Issues.Add(ABTJson::ObjectValue(MakeIssue(
                TEXT("warning"),
                TEXT("static_mesh"),
                AssetPath,
                FString::Printf(TEXT("Static mesh LOD0 has %d triangles."), TrianglesLod0),
                TEXT("Add LODs, simplify mesh, or create platform-specific mesh variants."),
                Targets.Names)));
        }

        if (LODCount <= 1 && TrianglesLod0 > 10000)
        {
            ++Stats.Issues;
            Issues.Add(ABTJson::ObjectValue(MakeIssue(
                TEXT("info"),
                TEXT("static_mesh"),
                AssetPath,
                TEXT("Static mesh has only one LOD."),
                TEXT("Generate LODs for mid/far distances, especially for Android."),
                Targets.Names)));
        }

        if (Targets.bAndroid && Mesh->NaniteSettings.bEnabled)
        {
            ++Stats.Issues;
            Issues.Add(ABTJson::ObjectValue(MakeIssue(
                TEXT("info"),
                TEXT("static_mesh"),
                AssetPath,
                TEXT("Nanite is enabled while Android is a target."),
                TEXT("Verify target Android GPU/RHI support and fallback mesh quality."),
                { TEXT("Android") })));
        }
    }

    void AnalyzeSkeletalMesh(
        USkeletalMesh* Mesh,
        const FABTPerformanceTargets& Targets,
        TArray<TSharedPtr<FJsonValue>>& Issues,
        FABTPerformanceStats& Stats)
    {
        if (!Mesh)
        {
            return;
        }

        ++Stats.SkeletalMeshes;
        const int32 LODCount = Mesh->GetLODNum();
        if (LODCount <= 1)
        {
            ++Stats.Issues;
            Issues.Add(ABTJson::ObjectValue(MakeIssue(
                TEXT("info"),
                TEXT("skeletal_mesh"),
                Mesh->GetPathName(),
                TEXT("Skeletal mesh has only one LOD."),
                TEXT("Add skeletal mesh LODs for distance and Android performance."),
                Targets.Names)));
        }
    }

    bool BuildOptimizationData(
        const TSharedPtr<FJsonObject>& Request,
        bool bIncludePlan,
        TSharedPtr<FJsonObject>& OutJson,
        FString& OutError)
    {
        const FABTPerformanceTargets Targets = ParseTargets(Request);
        const FString Profile = ABTJson::GetString(Request, TEXT("profile"), TEXT("balanced"));
        const bool bAllowVisualChanges = ABTJson::GetBool(Request, TEXT("allowVisualChanges"), false);
        const bool bIncludeAssets = ABTJson::GetBool(Request, TEXT("includeAssets"), true);
        const bool bIncludeMaterials = ABTJson::GetBool(Request, TEXT("includeMaterials"), true);
        const bool bIncludeMeshes = ABTJson::GetBool(Request, TEXT("includeMeshes"), true);
        const bool bIncludeProjectSettings = ABTJson::GetBool(Request, TEXT("includeProjectSettings"), true);
        const int32 MaxAssets = ABTJson::GetInt(Request, TEXT("maxAssets"), 2000);

        const TArray<TSharedPtr<FJsonValue>>* RequestedAssetPathValues = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* RequestedContentPathValues = nullptr;
        const bool bHasAssetPaths = Request.IsValid() && Request->TryGetArrayField(TEXT("assetPaths"), RequestedAssetPathValues);
        const bool bHasContentPaths = Request.IsValid() && Request->TryGetArrayField(TEXT("contentPaths"), RequestedContentPathValues);
        TArray<FString> AssetPaths = ParseAssetPaths(Request, OutError);
        if (!OutError.IsEmpty())
        {
            return false;
        }

        TArray<FString> ContentPaths = ParseContentPaths(Request, OutError, !bHasAssetPaths || bHasContentPaths);
        if (!OutError.IsEmpty())
        {
            return false;
        }

        const FABTPerformanceThresholds Thresholds = GetThresholds(Targets, Profile, bAllowVisualChanges);
        TArray<FAssetData> AssetData;
        if (ContentPaths.Num() > 0 && !CollectAssets(ContentPaths, MaxAssets, AssetData, OutError))
        {
            return false;
        }
        if (AssetPaths.Num() > 0 && !CollectExactAssets(AssetPaths, MaxAssets, AssetData, OutError))
        {
            return false;
        }
        DeduplicateAssets(AssetData);
        if (AssetData.Num() > MaxAssets)
        {
            AssetData.SetNum(MaxAssets);
        }

        FABTPerformanceStats Stats;
        Stats.bAssetLimitHit = AssetData.Num() >= MaxAssets;
        TArray<TSharedPtr<FJsonValue>> Issues;
        TArray<TSharedPtr<FJsonValue>> Plan;

        for (const FAssetData& Data : AssetData)
        {
            UObject* Asset = Data.GetAsset();
            if (!Asset)
            {
                continue;
            }

            ++Stats.ScannedAssets;

            if (bIncludeAssets)
            {
                if (UTexture2D* Texture = Cast<UTexture2D>(Asset))
                {
                    AnalyzeTexture(Texture, Targets, Thresholds, Issues, bIncludePlan ? &Plan : nullptr, Stats);
                    continue;
                }
            }

            if (bIncludeMaterials)
            {
                if (UMaterial* Material = Cast<UMaterial>(Asset))
                {
                    AnalyzeMaterial(Material, Targets, Thresholds, Issues, bIncludePlan ? &Plan : nullptr, Stats);
                    continue;
                }
                if (UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(Asset))
                {
                    AnalyzeMaterialInstance(MaterialInstance, Issues, Stats, Targets);
                    continue;
                }
            }

            if (bIncludeMeshes)
            {
                if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
                {
                    AnalyzeStaticMesh(StaticMesh, Targets, Thresholds, Issues, Stats);
                    continue;
                }
                if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset))
                {
                    AnalyzeSkeletalMesh(SkeletalMesh, Targets, Issues, Stats);
                    continue;
                }
            }
        }

        if (bIncludePlan && bIncludeProjectSettings)
        {
            const int32 PlanBeforeConfig = Plan.Num();
            AddProjectSettingActions(Targets, Profile, Plan);
            Stats.PlannedActions += Plan.Num() - PlanBeforeConfig;
        }

        OutJson = ABTJson::Ok();
        OutJson->SetStringField(TEXT("profile"), Profile);
        OutJson->SetArrayField(TEXT("targets"), StringArrayToJson(Targets.Names));
        OutJson->SetArrayField(TEXT("content_paths"), StringArrayToJson(ContentPaths));
        OutJson->SetArrayField(TEXT("asset_paths"), StringArrayToJson(AssetPaths));

        TSharedPtr<FJsonObject> ThresholdJson = ABTJson::Object();
        ThresholdJson->SetNumberField(TEXT("max_texture_size"), Thresholds.MaxTextureSize);
        ThresholdJson->SetNumberField(TEXT("material_expression_warning"), Thresholds.MaterialExpressionWarning);
        ThresholdJson->SetNumberField(TEXT("static_mesh_triangle_warning"), Thresholds.StaticMeshTriangleWarning);
        ThresholdJson->SetBoolField(TEXT("allow_visual_changes"), Thresholds.bAllowVisualChanges);
        OutJson->SetObjectField(TEXT("thresholds"), ThresholdJson);

        TSharedPtr<FJsonObject> Summary = ABTJson::Object();
        Summary->SetNumberField(TEXT("scanned_assets"), Stats.ScannedAssets);
        Summary->SetNumberField(TEXT("textures"), Stats.Textures);
        Summary->SetNumberField(TEXT("materials"), Stats.Materials);
        Summary->SetNumberField(TEXT("material_instances"), Stats.MaterialInstances);
        Summary->SetNumberField(TEXT("static_meshes"), Stats.StaticMeshes);
        Summary->SetNumberField(TEXT("skeletal_meshes"), Stats.SkeletalMeshes);
        Summary->SetNumberField(TEXT("issues"), Stats.Issues);
        Summary->SetNumberField(TEXT("planned_actions"), bIncludePlan ? Plan.Num() : 0);
        Summary->SetBoolField(TEXT("asset_limit_hit"), Stats.bAssetLimitHit);
        OutJson->SetObjectField(TEXT("summary"), Summary);
        OutJson->SetArrayField(TEXT("issues"), Issues);
        if (bIncludePlan)
        {
            OutJson->SetArrayField(TEXT("plan"), Plan);
        }
        return true;
    }

    bool ApplyTextureAction(const TSharedPtr<FJsonObject>& Action, bool bSaveAssets, TArray<TSharedPtr<FJsonValue>>& Applied, FString& OutError)
    {
        const FString AssetPath = ABTJson::GetString(Action, TEXT("asset_path"));
        UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *AssetPath);
        if (!Texture)
        {
            OutError = FString::Printf(TEXT("Could not load texture: %s"), *AssetPath);
            return false;
        }

        const FString Op = ABTJson::GetString(Action, TEXT("op"));
        Texture->Modify();
        Texture->PreEditChange(nullptr);

        if (Op == TEXT("set_texture_max_size"))
        {
            Texture->MaxTextureSize = FCString::Atoi(*ABTJson::GetString(Action, TEXT("to")));
        }
        else if (Op == TEXT("set_texture_mips_from_group"))
        {
            Texture->MipGenSettings = TMGS_FromTextureGroup;
        }
        else if (Op == TEXT("allow_texture_streaming"))
        {
            Texture->NeverStream = false;
        }
        else if (Op == TEXT("disable_texture_virtual_streaming"))
        {
            Texture->VirtualTextureStreaming = false;
        }
        else if (Op == TEXT("set_mask_texture_settings"))
        {
            Texture->CompressionSettings = TC_Masks;
            Texture->SRGB = false;
        }
        else
        {
            OutError = FString::Printf(TEXT("Unsupported texture optimization op: %s"), *Op);
            return false;
        }

        Texture->PostEditChange();
        Texture->MarkPackageDirty();
        Texture->UpdateResource();
        if (bSaveAssets)
        {
            UEditorAssetLibrary::SaveLoadedAsset(Texture, false);
        }

        Applied.Add(ABTJson::ObjectValue(Action));
        return true;
    }

    bool ApplyMaterialAction(const TSharedPtr<FJsonObject>& Action, bool bSaveAssets, TArray<TSharedPtr<FJsonValue>>& Applied, FString& OutError)
    {
        const FString AssetPath = ABTJson::GetString(Action, TEXT("asset_path"));
        UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
        if (!Material)
        {
            OutError = FString::Printf(TEXT("Could not load material: %s"), *AssetPath);
            return false;
        }

        const FString Op = ABTJson::GetString(Action, TEXT("op"));
        Material->Modify();
        Material->PreEditChange(nullptr);

        if (Op == TEXT("disable_material_two_sided"))
        {
            Material->TwoSided = false;
        }
        else if (Op == TEXT("set_material_fully_rough"))
        {
            Material->bFullyRough = true;
        }
        else
        {
            OutError = FString::Printf(TEXT("Unsupported material optimization op: %s"), *Op);
            return false;
        }

        Material->PostEditChange();
        UMaterialEditingLibrary::RecompileMaterial(Material);
        Material->MarkPackageDirty();
        if (bSaveAssets)
        {
            UEditorAssetLibrary::SaveLoadedAsset(Material, false);
        }

        Applied.Add(ABTJson::ObjectValue(Action));
        return true;
    }

    bool ApplyConfigAction(const TSharedPtr<FJsonObject>& Action, TArray<TSharedPtr<FJsonValue>>& Applied, FString& OutError)
    {
        const FString Op = ABTJson::GetString(Action, TEXT("op"));
        bool bWrote = false;

        if (Op == TEXT("set_device_profile_cvar"))
        {
            bWrote = WriteDeviceProfileCVarValue(
                ABTJson::GetString(Action, TEXT("file")),
                ABTJson::GetString(Action, TEXT("section")),
                ABTJson::GetString(Action, TEXT("cvar")),
                ABTJson::GetString(Action, TEXT("to")),
                OutError);
        }
        else
        {
            bWrote = WriteConfigValue(
                ABTJson::GetString(Action, TEXT("file")),
                ABTJson::GetString(Action, TEXT("section")),
                ABTJson::GetString(Action, TEXT("key")),
                ABTJson::GetString(Action, TEXT("to")),
                OutError);
        }

        if (!bWrote)
        {
            return false;
        }

        Applied.Add(ABTJson::ObjectValue(Action));
        return true;
    }
}

bool FABTPerformanceTools::AnalyzeProject(const TSharedPtr<FJsonObject>& Request, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    return BuildOptimizationData(Request, false, OutJson, OutError);
}

bool FABTPerformanceTools::DryRunOptimization(const TSharedPtr<FJsonObject>& Request, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    return BuildOptimizationData(Request, true, OutJson, OutError);
}

bool FABTPerformanceTools::ApplyOptimization(const TSharedPtr<FJsonObject>& Request, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    const bool bSaveAssets = ABTJson::GetBool(Request, TEXT("saveAssets"), false);
    const bool bWriteConfig = ABTJson::GetBool(Request, TEXT("writeConfig"), true);
    const bool bApplyVisualChanges = ABTJson::GetBool(Request, TEXT("applyVisualChanges"), false);
    const int32 MaxActions = ABTJson::GetInt(Request, TEXT("maxActions"), 200);

    if (bApplyVisualChanges && Request.IsValid())
    {
        Request->SetBoolField(TEXT("allowVisualChanges"), true);
    }

    TSharedPtr<FJsonObject> PlanJson;
    if (!BuildOptimizationData(Request, true, PlanJson, OutError))
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Plan = nullptr;
    if (!PlanJson->TryGetArrayField(TEXT("plan"), Plan) || !Plan)
    {
        OutError = TEXT("Generated performance plan is missing");
        return false;
    }

    const FScopedTransaction Transaction(NSLOCTEXT("AgentBlueprintTools", "ApplyPerformanceOptimization", "Apply Performance Optimization"));
    TArray<TSharedPtr<FJsonValue>> Applied;
    TArray<TSharedPtr<FJsonValue>> Skipped;

    int32 AppliedCount = 0;
    for (const TSharedPtr<FJsonValue>& Value : *Plan)
    {
        if (AppliedCount >= MaxActions)
        {
            TSharedPtr<FJsonObject> Skip = ABTJson::Object();
            Skip->SetStringField(TEXT("reason"), TEXT("maxActions reached"));
            Skipped.Add(ABTJson::ObjectValue(Skip));
            break;
        }

        const TSharedPtr<FJsonObject> Action = Value->AsObject();
        if (!Action.IsValid())
        {
            continue;
        }

        if (ABTJson::GetBool(Action, TEXT("requires_visual_review"), false) && !bApplyVisualChanges)
        {
            TSharedPtr<FJsonObject> Skip = ABTJson::Object();
            Skip->SetStringField(TEXT("reason"), TEXT("requires applyVisualChanges=true"));
            Skip->SetObjectField(TEXT("action"), Action);
            Skipped.Add(ABTJson::ObjectValue(Skip));
            continue;
        }

        const FString Category = ABTJson::GetString(Action, TEXT("category"));
        bool bApplied = false;
        if (Category == TEXT("texture"))
        {
            bApplied = ApplyTextureAction(Action, bSaveAssets, Applied, OutError);
        }
        else if (Category == TEXT("material"))
        {
            bApplied = ApplyMaterialAction(Action, bSaveAssets, Applied, OutError);
        }
        else if (Category == TEXT("project_setting"))
        {
            if (bWriteConfig)
            {
                bApplied = ApplyConfigAction(Action, Applied, OutError);
            }
            else
            {
                TSharedPtr<FJsonObject> Skip = ABTJson::Object();
                Skip->SetStringField(TEXT("reason"), TEXT("writeConfig=false"));
                Skip->SetObjectField(TEXT("action"), Action);
                Skipped.Add(ABTJson::ObjectValue(Skip));
                continue;
            }
        }
        else
        {
            TSharedPtr<FJsonObject> Skip = ABTJson::Object();
            Skip->SetStringField(TEXT("reason"), FString::Printf(TEXT("unsupported action category: %s"), *Category));
            Skip->SetObjectField(TEXT("action"), Action);
            Skipped.Add(ABTJson::ObjectValue(Skip));
            continue;
        }

        if (!bApplied)
        {
            return false;
        }
        ++AppliedCount;
    }

    OutJson = ABTJson::Ok();
    OutJson->SetObjectField(TEXT("analysis"), PlanJson);
    OutJson->SetArrayField(TEXT("applied"), Applied);
    OutJson->SetArrayField(TEXT("skipped"), Skipped);
    OutJson->SetNumberField(TEXT("applied_count"), Applied.Num());
    OutJson->SetNumberField(TEXT("skipped_count"), Skipped.Num());
    OutJson->SetBoolField(TEXT("saved_assets"), bSaveAssets);
    OutJson->SetBoolField(TEXT("wrote_config"), bWriteConfig);
    return true;
}
