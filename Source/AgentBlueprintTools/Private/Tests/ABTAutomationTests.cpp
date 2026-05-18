#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Assets/ABTAssetTools.h"
#include "Blueprint/ABTBlueprintTools.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "GameFramework/Actor.h"
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
    constexpr const TCHAR* TestInterfacePath = TEXT("/Game/ABT_Automation/BPI_ABT_RoundTrip");
    constexpr const TCHAR* TestStructPath = TEXT("/Game/ABT_Automation/ST_ABT_MoveConfig_RoundTrip");
    constexpr const TCHAR* TestWidgetPath = TEXT("/Game/ABT_Automation/WBP_ABT_ButtonPages_RoundTrip");
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

    bool JsonArrayObjectNumberAtLeast(
        const TSharedPtr<FJsonObject>& Json,
        const FString& ArrayName,
        const FString& MatchField,
        const FString& MatchValue,
        const FString& NumberField,
        double Minimum)
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
            double NumberValue = 0.0;
            if (Object.IsValid() &&
                Object->TryGetStringField(MatchField, Actual) &&
                Actual == MatchValue &&
                Object->TryGetNumberField(NumberField, NumberValue))
            {
                return NumberValue >= Minimum;
            }
        }
        return false;
    }

    bool JsonArrayObjectContainsNestedString(
        const TSharedPtr<FJsonObject>& Json,
        const FString& ArrayName,
        const FString& MatchField,
        const FString& MatchValue,
        const FString& NestedArrayName,
        const FString& NestedField,
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
            if (!Object.IsValid() || !Object->TryGetStringField(MatchField, Actual) || Actual != MatchValue)
            {
                continue;
            }

            const TArray<TSharedPtr<FJsonValue>>* NestedValues = nullptr;
            if (!Object->TryGetArrayField(NestedArrayName, NestedValues) || !NestedValues)
            {
                return false;
            }

            for (const TSharedPtr<FJsonValue>& NestedValue : *NestedValues)
            {
                const TSharedPtr<FJsonObject> NestedObject = NestedValue->AsObject();
                FString NestedActual;
                if (NestedObject.IsValid() && NestedObject->TryGetStringField(NestedField, NestedActual) && NestedActual == Expected)
                {
                    return true;
                }
            }
            return false;
        }
        return false;
    }

    bool JsonObjectArrayContainsString(
        const TSharedPtr<FJsonObject>& Json,
        const FString& ObjectName,
        const FString& ArrayName,
        const FString& Expected)
    {
        const TSharedPtr<FJsonObject>* Object = nullptr;
        if (!Json.IsValid() || !Json->TryGetObjectField(ObjectName, Object) || !Object || !Object->IsValid())
        {
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!(*Object)->TryGetArrayField(ArrayName, Values) || !Values)
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            if (Value->AsString() == Expected)
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
    DeleteAssetIfExists(TestInterfacePath);
    DeleteAssetIfExists(TestStructPath);
    DeleteAssetIfExists(TestWidgetPath);
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

    TSharedPtr<FJsonObject> LoopPatch = ABTJson::Object();
    LoopPatch->SetStringField(TEXT("target"), BlueprintPath);
    TArray<TSharedPtr<FJsonValue>> LoopOps;
    TSharedPtr<FJsonObject> LoopOp = MakeOp(TEXT("ensure_looping_move"));
    LoopOp->SetStringField(TEXT("function"), TEXT("MoveByDelta"));
    LoopOp->SetNumberField(TEXT("interval"), 0.15);
    TArray<TSharedPtr<FJsonValue>> Delta;
    Delta.Add(MakeShared<FJsonValueNumber>(25.0));
    Delta.Add(MakeShared<FJsonValueNumber>(0.0));
    Delta.Add(MakeShared<FJsonValueNumber>(0.0));
    LoopOp->SetArrayField(TEXT("delta"), Delta);
    LoopOps.Add(ABTJson::ObjectValue(LoopOp));
    LoopPatch->SetArrayField(TEXT("operations"), LoopOps);

    TestTrue(TEXT("Dry-run looping move patch"), FABTBlueprintTools::DryRunPatch(LoopPatch, Json, Error));
    TestTrue(TEXT("Looping move patch is valid"), Json->GetBoolField(TEXT("valid")));
    TestTrue(TEXT("Apply looping move patch"), FABTBlueprintTools::ApplyPatch(LoopPatch, true, Json, Error));
    TestTrue(TEXT("Looping move Blueprint compile ok"), Json->GetObjectField(TEXT("compile"))->GetBoolField(TEXT("compile_ok")));

    FABTBlueprintExportOptions SummaryOptions;
    SummaryOptions.bIncludeGraphs = false;
    SummaryOptions.bIncludePins = false;
    TestTrue(TEXT("Read compact Blueprint summary"), FABTBlueprintTools::ExportBlueprint(BlueprintPath, Json, Error, SummaryOptions));
    TestTrue(TEXT("Summary includes timer call"), JsonObjectArrayContainsString(Json, TEXT("semantic_summary"), TEXT("external_calls"), TEXT("K2_SetTimer")));
    TestTrue(TEXT("Summary includes movement call"), JsonObjectArrayContainsString(Json, TEXT("semantic_summary"), TEXT("external_calls"), TEXT("K2_AddActorWorldOffset")));

    TSharedPtr<FJsonObject> PlaceRequest = ABTJson::Object();
    PlaceRequest->SetStringField(TEXT("assetPath"), BlueprintPath);
    PlaceRequest->SetStringField(TEXT("label"), TEXT("ABT_RoundTrip_TransientMover"));
    PlaceRequest->SetBoolField(TEXT("transient"), true);
    TArray<TSharedPtr<FJsonValue>> Location;
    Location.Add(MakeShared<FJsonValueNumber>(120.0));
    Location.Add(MakeShared<FJsonValueNumber>(0.0));
    Location.Add(MakeShared<FJsonValueNumber>(80.0));
    PlaceRequest->SetArrayField(TEXT("location"), Location);
    TestTrue(TEXT("Place transient moving actor"), FABTAssetTools::PlaceActor(PlaceRequest, Json, Error));
    TestTrue(TEXT("Placed actor is transient"), Json->GetBoolField(TEXT("transient")));
    const FString ActorPath = ABTJson::GetString(Json, TEXT("actor_path"));
    if (AActor* Actor = FindObject<AActor>(nullptr, *ActorPath))
    {
        Actor->Destroy();
    }

    TSharedPtr<FJsonObject> StructRequest = MakeCreateAssetRequest(TEXT("UserDefinedStruct"), TestStructPath);
    TArray<TSharedPtr<FJsonValue>> Fields;
    TSharedPtr<FJsonObject> SpeedField = ABTJson::Object();
    SpeedField->SetStringField(TEXT("name"), TEXT("Speed"));
    SpeedField->SetStringField(TEXT("type"), TEXT("float"));
    SpeedField->SetStringField(TEXT("default"), TEXT("300.0"));
    Fields.Add(ABTJson::ObjectValue(SpeedField));
    TSharedPtr<FJsonObject> DeltaField = ABTJson::Object();
    DeltaField->SetStringField(TEXT("name"), TEXT("Delta"));
    DeltaField->SetStringField(TEXT("type"), TEXT("vector"));
    DeltaField->SetStringField(TEXT("default"), TEXT("(X=25.0,Y=0.0,Z=0.0)"));
    Fields.Add(ABTJson::ObjectValue(DeltaField));
    StructRequest->SetArrayField(TEXT("fields"), Fields);
    TestTrue(TEXT("Create UserDefinedStruct asset"), FABTAssetTools::CreateAsset(StructRequest, Json, Error));
    TSharedPtr<FJsonObject> ReadStructRequest = ABTJson::Object();
    ReadStructRequest->SetStringField(TEXT("assetPath"), ABTJson::GetString(Json, TEXT("asset_path")));
    TestTrue(TEXT("Read UserDefinedStruct asset"), FABTAssetTools::ReadAsset(ReadStructRequest, Json, Error));
    TestTrue(TEXT("Struct includes Speed field"), JsonArrayContainsObjectString(Json, TEXT("fields"), TEXT("name"), TEXT("Speed")));

    TSharedPtr<FJsonObject> WidgetRequest = MakeCreateAssetRequest(TEXT("WidgetBlueprint"), TestWidgetPath);
    WidgetRequest->SetStringField(TEXT("parentClass"), TEXT("/Script/AIAgentTool.ABTButtonPagesWidget"));
    TestTrue(TEXT("Create Widget Blueprint asset"), FABTAssetTools::CreateAsset(WidgetRequest, Json, Error));
    const FString WidgetPath = ABTJson::GetString(Json, TEXT("asset_path"));

    TSharedPtr<FJsonObject> WidgetPatch = ABTJson::Object();
    WidgetPatch->SetStringField(TEXT("target"), WidgetPath);
    TArray<TSharedPtr<FJsonValue>> WidgetOps;
    TSharedPtr<FJsonObject> WidgetOp = MakeOp(TEXT("configure_button_pages_widget"));
    TArray<TSharedPtr<FJsonValue>> Pages;
    for (int32 Index = 0; Index < 3; ++Index)
    {
        TSharedPtr<FJsonObject> Page = ABTJson::Object();
        Page->SetStringField(TEXT("buttonText"), FString::Printf(TEXT("Tab %d"), Index + 1));
        Page->SetStringField(TEXT("title"), FString::Printf(TEXT("Page %d"), Index + 1));
        Page->SetStringField(TEXT("body"), TEXT("Automation page"));
        Pages.Add(ABTJson::ObjectValue(Page));
    }
    WidgetOp->SetArrayField(TEXT("pages"), Pages);
    WidgetOps.Add(ABTJson::ObjectValue(WidgetOp));
    WidgetPatch->SetArrayField(TEXT("operations"), WidgetOps);
    TestTrue(TEXT("Dry-run Widget patch"), FABTBlueprintTools::DryRunPatch(WidgetPatch, Json, Error));
    TestTrue(TEXT("Widget patch is valid"), Json->GetBoolField(TEXT("valid")));
    TestTrue(TEXT("Apply Widget patch"), FABTBlueprintTools::ApplyPatch(WidgetPatch, true, Json, Error));
    TestTrue(TEXT("Widget Blueprint compile ok"), Json->GetObjectField(TEXT("compile"))->GetBoolField(TEXT("compile_ok")));
    TestTrue(TEXT("Read Widget Blueprint IR"), FABTBlueprintTools::ExportBlueprint(WidgetPath, Json, Error));
    TestTrue(TEXT("Widget has first page button"), JsonArrayContainsObjectString(Json, TEXT("widgets"), TEXT("name"), TEXT("PageButton_0")));
    TestTrue(TEXT("Widget has first popup page"), JsonArrayContainsObjectString(Json, TEXT("widgets"), TEXT("name"), TEXT("PagePanel_0")));
    TestTrue(TEXT("Widget has first popup animation"), JsonArrayContainsObjectString(Json, TEXT("animations"), TEXT("name"), TEXT("PagePopup_0")));
    TestTrue(TEXT("Popup animation binds first page panel"), JsonArrayObjectContainsNestedString(Json, TEXT("animations"), TEXT("name"), TEXT("PagePopup_0"), TEXT("widget_bindings"), TEXT("widget_name"), TEXT("PagePanel_0")));
    TestTrue(TEXT("Popup animation has midpoint event key"), JsonArrayObjectNumberAtLeast(Json, TEXT("animations"), TEXT("name"), TEXT("PagePopup_0"), TEXT("event_key_count"), 1.0));

    TestTrue(TEXT("Create Blueprint Interface asset"), FABTAssetTools::CreateAsset(MakeCreateAssetRequest(TEXT("BlueprintInterface"), TestInterfacePath), Json, Error));
    TSharedPtr<FJsonObject> ReadInterfaceRequest = ABTJson::Object();
    ReadInterfaceRequest->SetStringField(TEXT("assetPath"), ABTJson::GetString(Json, TEXT("asset_path")));
    TestTrue(TEXT("Read Blueprint Interface asset"), FABTAssetTools::ReadAsset(ReadInterfaceRequest, Json, Error));
    TestEqual(TEXT("Interface blueprint type"), ABTJson::GetString(Json, TEXT("blueprint_type")), FString(TEXT("BPTYPE_Interface")));

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
