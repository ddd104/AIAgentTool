#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Assets/ABTAssetTools.h"
#include "Blueprint/ABTBlueprintTools.h"
#include "EditorAssetLibrary.h"
#include "Materials/ABTMaterialTools.h"
#include "Utils/ABTJson.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FABTBridgePatchShapeTest, "AgentBlueprintTools.PatchShape.DryRunRequiresTarget", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FABTBridgePatchShapeTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Patch = MakeShared<FJsonObject>();
    Patch->SetArrayField(TEXT("operations"), TArray<TSharedPtr<FJsonValue>>{});

    TSharedPtr<FJsonObject> OutJson;
    FString Error;
    const bool bOk = FABTBlueprintTools::DryRunPatch(Patch, OutJson, Error);

    TestTrue(TEXT("DryRunPatch should return a response"), bOk);
    TestTrue(TEXT("OutJson should be valid"), OutJson.IsValid());
    TestFalse(TEXT("Patch without target should be invalid"), OutJson->GetBoolField(TEXT("valid")));
    return true;
}

namespace
{
    constexpr const TCHAR* TestRoot = TEXT("/Game/ABT_Automation");
    constexpr const TCHAR* TestBlueprintPath = TEXT("/Game/ABT_Automation/BP_ABT_RoundTrip");
    constexpr const TCHAR* TestMaterialPath = TEXT("/Game/ABT_Automation/M_ABT_RoundTrip");
    constexpr const TCHAR* TestMaterialInstancePath = TEXT("/Game/ABT_Automation/MI_ABT_RoundTrip");

    TSharedPtr<FJsonObject> MakeCreateAssetRequest(const FString& AssetType, const FString& Path)
    {
        TSharedPtr<FJsonObject> Request = ABTJson::Object();
        Request->SetStringField(TEXT("assetType"), AssetType);
        Request->SetStringField(TEXT("path"), Path);
        Request->SetBoolField(TEXT("save"), true);
        return Request;
    }

    TSharedPtr<FJsonObject> MakeOp(const FString& Name)
    {
        TSharedPtr<FJsonObject> Op = ABTJson::Object();
        Op->SetStringField(TEXT("op"), Name);
        return Op;
    }

    bool JsonArrayContainsObjectString(
        const TSharedPtr<FJsonObject>& Json,
        const FString& ArrayName,
        const FString& FieldName,
        const FString& Expected)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Json.IsValid() || !Json->TryGetArrayField(ArrayName, Values) || !Values)
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            const TSharedPtr<FJsonObject> Object = Value->AsObject();
            FString Actual;
            if (Object.IsValid() && Object->TryGetStringField(FieldName, Actual) && Actual == Expected)
            {
                return true;
            }
        }
        return false;
    }

    void DeleteAssetIfExists(const FString& AssetPath)
    {
        if (UEditorAssetLibrary::DoesAssetExist(AssetPath))
        {
            UEditorAssetLibrary::DeleteAsset(AssetPath);
        }
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FABTAssetBlueprintMaterialRoundTripTest, "AgentBlueprintTools.RoundTrip.AssetBlueprintMaterial", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FABTAssetBlueprintMaterialRoundTripTest::RunTest(const FString& Parameters)
{
    DeleteAssetIfExists(TestMaterialInstancePath);
    DeleteAssetIfExists(TestBlueprintPath);
    DeleteAssetIfExists(TestMaterialPath);
    UEditorAssetLibrary::MakeDirectory(TestRoot);

    FString Error;
    TSharedPtr<FJsonObject> Json;

    TestTrue(TEXT("Create Blueprint asset"), FABTAssetTools::CreateAsset(MakeCreateAssetRequest(TEXT("Blueprint"), TestBlueprintPath), Json, Error));
    if (!Json.IsValid())
    {
        AddError(TEXT("Blueprint creation did not return JSON"));
        return false;
    }
    const FString BlueprintPath = ABTJson::GetString(Json, TEXT("asset_path"));

    TSharedPtr<FJsonObject> BlueprintPatch = ABTJson::Object();
    BlueprintPatch->SetStringField(TEXT("target"), BlueprintPath);
    TArray<TSharedPtr<FJsonValue>> BlueprintOps;

    TSharedPtr<FJsonObject> VarOp = MakeOp(TEXT("ensure_variable"));
    VarOp->SetStringField(TEXT("name"), TEXT("ABTHealth"));
    VarOp->SetStringField(TEXT("type"), TEXT("float"));
    BlueprintOps.Add(ABTJson::ObjectValue(VarOp));

    TSharedPtr<FJsonObject> FunctionOp = MakeOp(TEXT("ensure_function"));
    FunctionOp->SetStringField(TEXT("name"), TEXT("ABTCompute"));
    BlueprintOps.Add(ABTJson::ObjectValue(FunctionOp));

    BlueprintPatch->SetArrayField(TEXT("operations"), BlueprintOps);

    TestTrue(TEXT("Dry-run Blueprint patch"), FABTBlueprintTools::DryRunPatch(BlueprintPatch, Json, Error));
    TestTrue(TEXT("Blueprint patch is valid"), Json->GetBoolField(TEXT("valid")));
    TestTrue(TEXT("Apply Blueprint patch"), FABTBlueprintTools::ApplyPatch(BlueprintPatch, true, Json, Error));
    TestTrue(TEXT("Blueprint compile after patch"), Json->GetObjectField(TEXT("compile"))->GetBoolField(TEXT("compile_ok")));

    TSharedPtr<FJsonObject> PropertyRequest = ABTJson::Object();
    PropertyRequest->SetStringField(TEXT("assetPath"), BlueprintPath);
    PropertyRequest->SetStringField(TEXT("object"), TEXT("DefaultObject"));
    PropertyRequest->SetStringField(TEXT("propertyPath"), TEXT("bCanBeDamaged"));
    PropertyRequest->SetBoolField(TEXT("value"), false);
    PropertyRequest->SetBoolField(TEXT("save"), true);
    TestTrue(TEXT("Set Blueprint CDO property"), FABTAssetTools::SetAssetProperty(PropertyRequest, Json, Error));

    TestTrue(TEXT("Read Blueprint IR"), FABTBlueprintTools::ExportBlueprint(BlueprintPath, Json, Error));
    TestTrue(TEXT("Blueprint IR includes variable"), JsonArrayContainsObjectString(Json, TEXT("variables"), TEXT("name"), TEXT("ABTHealth")));

    TestTrue(TEXT("Create Material asset"), FABTAssetTools::CreateAsset(MakeCreateAssetRequest(TEXT("Material"), TestMaterialPath), Json, Error));
    const FString MaterialPath = ABTJson::GetString(Json, TEXT("asset_path"));

    TSharedPtr<FJsonObject> MaterialPatch = ABTJson::Object();
    MaterialPatch->SetStringField(TEXT("target"), MaterialPath);
    TArray<TSharedPtr<FJsonValue>> MaterialOps;

    TSharedPtr<FJsonObject> ScalarOp = MakeOp(TEXT("add_expression"));
    ScalarOp->SetStringField(TEXT("id"), TEXT("RoughnessParam"));
    ScalarOp->SetStringField(TEXT("class"), TEXT("ScalarParameter"));
    ScalarOp->SetStringField(TEXT("parameter_name"), TEXT("ABTRoughness"));
    ScalarOp->SetNumberField(TEXT("value"), 0.42);
    ScalarOp->SetNumberField(TEXT("x"), -300);
    ScalarOp->SetNumberField(TEXT("y"), 0);
    MaterialOps.Add(ABTJson::ObjectValue(ScalarOp));

    TSharedPtr<FJsonObject> ConnectOp = MakeOp(TEXT("connect_material_property"));
    ConnectOp->SetStringField(TEXT("from"), TEXT("RoughnessParam"));
    ConnectOp->SetStringField(TEXT("property"), TEXT("Roughness"));
    MaterialOps.Add(ABTJson::ObjectValue(ConnectOp));

    MaterialPatch->SetArrayField(TEXT("operations"), MaterialOps);
    TestTrue(TEXT("Patch Material graph"), FABTMaterialTools::PatchMaterial(MaterialPatch, true, Json, Error));
    TestTrue(TEXT("Read Material"), FABTMaterialTools::ReadMaterial(MaterialPath, Json, Error));
    TestTrue(TEXT("Material has expressions"), Json->GetArrayField(TEXT("expressions")).Num() > 0);

    TSharedPtr<FJsonObject> MICRequest = MakeCreateAssetRequest(TEXT("MaterialInstanceConstant"), TestMaterialInstancePath);
    MICRequest->SetStringField(TEXT("parentMaterial"), MaterialPath);
    TestTrue(TEXT("Create Material Instance"), FABTAssetTools::CreateAsset(MICRequest, Json, Error));
    const FString MICPath = ABTJson::GetString(Json, TEXT("asset_path"));

    TSharedPtr<FJsonObject> MICPatch = ABTJson::Object();
    MICPatch->SetStringField(TEXT("target"), MICPath);
    TArray<TSharedPtr<FJsonValue>> MICOps;
    TSharedPtr<FJsonObject> SetScalarOp = MakeOp(TEXT("set_scalar_parameter"));
    SetScalarOp->SetStringField(TEXT("name"), TEXT("ABTRoughness"));
    SetScalarOp->SetNumberField(TEXT("value"), 0.75);
    MICOps.Add(ABTJson::ObjectValue(SetScalarOp));
    MICPatch->SetArrayField(TEXT("operations"), MICOps);

    TestTrue(TEXT("Patch Material Instance parameter"), FABTMaterialTools::PatchMaterial(MICPatch, true, Json, Error));
    TestTrue(TEXT("Read Material Instance"), FABTMaterialTools::ReadMaterial(MICPath, Json, Error));
    TestTrue(TEXT("Material Instance exposes scalar parameter"), JsonArrayContainsObjectString(Json, TEXT("scalar_parameters"), TEXT("name"), TEXT("ABTRoughness")));

    TestTrue(TEXT("Compile Blueprint asset"), FABTBlueprintTools::CompileBlueprintAsset(BlueprintPath, Json, Error));
    TestTrue(TEXT("Compiled Blueprint status ok"), Json->GetBoolField(TEXT("compile_ok")));

    return true;
}

#endif
