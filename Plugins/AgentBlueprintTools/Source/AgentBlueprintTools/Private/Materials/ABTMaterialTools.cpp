#include "Materials/ABTMaterialTools.h"
#include "Utils/ABTJson.h"

#include "EditorAssetLibrary.h"
#include "MaterialEditingLibrary.h"
#include "MaterialDomain.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSphereMask.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "ScopedTransaction.h"

namespace
{
    EMaterialProperty ResolveMaterialProperty(const FString& Name)
    {
        if (Name.Equals(TEXT("BaseColor"), ESearchCase::IgnoreCase)) return MP_BaseColor;
        if (Name.Equals(TEXT("Metallic"), ESearchCase::IgnoreCase)) return MP_Metallic;
        if (Name.Equals(TEXT("Specular"), ESearchCase::IgnoreCase)) return MP_Specular;
        if (Name.Equals(TEXT("Roughness"), ESearchCase::IgnoreCase)) return MP_Roughness;
        if (Name.Equals(TEXT("EmissiveColor"), ESearchCase::IgnoreCase)) return MP_EmissiveColor;
        if (Name.Equals(TEXT("Opacity"), ESearchCase::IgnoreCase)) return MP_Opacity;
        if (Name.Equals(TEXT("Normal"), ESearchCase::IgnoreCase)) return MP_Normal;
        return MP_BaseColor;
    }

    FString MaterialDomainToString(EMaterialDomain Domain)
    {
        switch (Domain)
        {
            case MD_Surface: return TEXT("Surface");
            case MD_DeferredDecal: return TEXT("DeferredDecal");
            case MD_LightFunction: return TEXT("LightFunction");
            case MD_Volume: return TEXT("Volume");
            case MD_PostProcess: return TEXT("PostProcess");
            case MD_UI: return TEXT("UI");
            default: return TEXT("Unknown");
        }
    }

    FString BlendModeToString(EBlendMode BlendMode)
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

    UClass* ResolveExpressionClass(const FString& ClassName)
    {
        FString Full = ClassName;
        if (!Full.StartsWith(TEXT("MaterialExpression")))
        {
            Full = TEXT("MaterialExpression") + Full;
        }
        UClass* Class = LoadObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *Full));
        if (!Class)
        {
            for (TObjectIterator<UClass> It; It; ++It)
            {
                UClass* Candidate = *It;
                if (Candidate && Candidate->GetName().Equals(Full, ESearchCase::IgnoreCase))
                {
                    Class = Candidate;
                    break;
                }
            }
        }
        return Class && Class->IsChildOf(UMaterialExpression::StaticClass()) ? Class : nullptr;
    }

    bool SplitExpressionPin(const FString& Ref, FString& OutId, FString& OutOutput)
    {
        if (!Ref.Split(TEXT("."), &OutId, &OutOutput))
        {
            OutId = Ref;
            OutOutput = TEXT("");
        }
        return !OutId.IsEmpty();
    }

    FLinearColor ReadLinearColor(const TSharedPtr<FJsonObject>& Op, const FLinearColor& DefaultValue)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Op.IsValid() || !Op->TryGetArrayField(TEXT("value"), Values) || !Values || Values->Num() < 3)
        {
            return DefaultValue;
        }

        return FLinearColor(
            static_cast<float>((*Values)[0]->AsNumber()),
            static_cast<float>((*Values)[1]->AsNumber()),
            static_cast<float>((*Values)[2]->AsNumber()),
            Values->Num() > 3 ? static_cast<float>((*Values)[3]->AsNumber()) : 1.0f);
    }

    TSharedPtr<FJsonObject> ExportScalarParameter(const FScalarParameterValue& Param)
    {
        TSharedPtr<FJsonObject> Json = ABTJson::Object();
        Json->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
        Json->SetNumberField(TEXT("value"), Param.ParameterValue);
        return Json;
    }

    TSharedPtr<FJsonObject> ExportVectorParameter(const FVectorParameterValue& Param)
    {
        TSharedPtr<FJsonObject> Json = ABTJson::Object();
        Json->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
        TArray<TSharedPtr<FJsonValue>> Value;
        Value.Add(MakeShared<FJsonValueNumber>(Param.ParameterValue.R));
        Value.Add(MakeShared<FJsonValueNumber>(Param.ParameterValue.G));
        Value.Add(MakeShared<FJsonValueNumber>(Param.ParameterValue.B));
        Value.Add(MakeShared<FJsonValueNumber>(Param.ParameterValue.A));
        Json->SetArrayField(TEXT("value"), Value);
        return Json;
    }

    template <typename T>
    T* CreateExpression(UMaterial* Material, int32 X, int32 Y)
    {
        return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass(), X, Y));
    }

    bool BuildUICircleMaterial(UMaterial* Material, const TSharedPtr<FJsonObject>& Op, TArray<FString>& OutMessages, FString& OutError)
    {
        if (!Material)
        {
            OutError = TEXT("make_ui_circle requires a UMaterial or a material instance with a UMaterial parent");
            return false;
        }

        Material->Modify();
        UMaterialEditingLibrary::DeleteAllMaterialExpressions(Material);

        Material->MaterialDomain = MD_UI;
        Material->BlendMode = BLEND_Translucent;
        Material->SetShadingModel(MSM_Unlit);
        Material->TwoSided = true;

        UMaterialExpressionTextureCoordinate* UV = CreateExpression<UMaterialExpressionTextureCoordinate>(Material, -900, 0);
        UMaterialExpressionConstant2Vector* Center = CreateExpression<UMaterialExpressionConstant2Vector>(Material, -900, 170);
        UMaterialExpressionSphereMask* Mask = CreateExpression<UMaterialExpressionSphereMask>(Material, -620, 80);
        UMaterialExpressionVectorParameter* Color = CreateExpression<UMaterialExpressionVectorParameter>(Material, -620, -160);
        UMaterialExpressionMultiply* ColorTimesMask = CreateExpression<UMaterialExpressionMultiply>(Material, -330, -60);

        if (!UV || !Center || !Mask || !Color || !ColorTimesMask)
        {
            OutError = TEXT("Failed to create one or more UI circle material expressions");
            return false;
        }

        Center->R = 0.5f;
        Center->G = 0.5f;

        Mask->AttenuationRadius = static_cast<float>(ABTJson::GetNumber(Op, TEXT("radius"), 0.48));
        Mask->HardnessPercent = static_cast<float>(ABTJson::GetNumber(Op, TEXT("hardness"), 100.0));
        Mask->A.Connect(0, UV);
        Mask->B.Connect(0, Center);

        Color->ParameterName = *ABTJson::GetString(Op, TEXT("colorParameter"), TEXT("CircleColor"));
        Color->DefaultValue = ReadLinearColor(Op, FLinearColor::White);

        ColorTimesMask->A.Connect(0, Color);
        ColorTimesMask->B.Connect(0, Mask);

        if (!UMaterialEditingLibrary::ConnectMaterialProperty(ColorTimesMask, TEXT(""), MP_EmissiveColor))
        {
            OutError = TEXT("Failed to connect circle color to UI final color");
            return false;
        }

        if (!UMaterialEditingLibrary::ConnectMaterialProperty(Mask, TEXT(""), MP_Opacity))
        {
            OutError = TEXT("Failed to connect circle mask to UI opacity");
            return false;
        }

        Material->PreEditChange(nullptr);
        Material->PostEditChange();
        UMaterialEditingLibrary::RecompileMaterial(Material);

        OutMessages.Add(TEXT("Configured material as translucent UI circle"));
        return true;
    }
}

bool FABTMaterialTools::ReadMaterial(const FString& AssetPath, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    UObject* Object = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Object)
    {
        OutError = FString::Printf(TEXT("Could not load material asset: %s"), *AssetPath);
        return false;
    }

    OutJson = ABTJson::Ok();
    OutJson->SetStringField(TEXT("asset_path"), AssetPath);
    OutJson->SetStringField(TEXT("class"), Object->GetClass()->GetPathName());

    if (UMaterial* Material = Cast<UMaterial>(Object))
    {
        OutJson->SetStringField(TEXT("domain"), MaterialDomainToString(Material->MaterialDomain));
        OutJson->SetStringField(TEXT("blend_mode"), BlendModeToString(Material->BlendMode));
        OutJson->SetBoolField(TEXT("two_sided"), Material->TwoSided);

        TArray<TSharedPtr<FJsonValue>> Expressions;
        for (UMaterialExpression* Expr : Material->GetExpressions())
        {
            if (!Expr) continue;
            TSharedPtr<FJsonObject> E = ABTJson::Object();
            E->SetStringField(TEXT("name"), Expr->GetName());
            E->SetStringField(TEXT("class"), Expr->GetClass()->GetPathName());
            E->SetNumberField(TEXT("x"), Expr->MaterialExpressionEditorX);
            E->SetNumberField(TEXT("y"), Expr->MaterialExpressionEditorY);
            Expressions.Add(ABTJson::ObjectValue(E));
        }
        OutJson->SetArrayField(TEXT("expressions"), Expressions);
        return true;
    }

    if (UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Object))
    {
        OutJson->SetStringField(TEXT("parent"), MIC->Parent ? MIC->Parent->GetPathName() : TEXT(""));
        if (UMaterial* ParentMaterial = Cast<UMaterial>(MIC->Parent))
        {
            TSharedPtr<FJsonObject> Parent = ABTJson::Object();
            Parent->SetStringField(TEXT("domain"), MaterialDomainToString(ParentMaterial->MaterialDomain));
            Parent->SetStringField(TEXT("blend_mode"), BlendModeToString(ParentMaterial->BlendMode));
            Parent->SetNumberField(TEXT("expression_count"), ParentMaterial->GetExpressions().Num());
            OutJson->SetObjectField(TEXT("parent_material"), Parent);
        }

        TArray<TSharedPtr<FJsonValue>> ScalarParameters;
        for (const FScalarParameterValue& Param : MIC->ScalarParameterValues)
        {
            ScalarParameters.Add(ABTJson::ObjectValue(ExportScalarParameter(Param)));
        }
        OutJson->SetArrayField(TEXT("scalar_parameters"), ScalarParameters);

        TArray<TSharedPtr<FJsonValue>> VectorParameters;
        for (const FVectorParameterValue& Param : MIC->VectorParameterValues)
        {
            VectorParameters.Add(ABTJson::ObjectValue(ExportVectorParameter(Param)));
        }
        OutJson->SetArrayField(TEXT("vector_parameters"), VectorParameters);
        return true;
    }

    OutError = TEXT("Asset is not a UMaterial or UMaterialInstanceConstant");
    return false;
}

bool FABTMaterialTools::PatchMaterial(const TSharedPtr<FJsonObject>& Patch, bool bSaveOnSuccess, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
    const FString Target = ABTJson::GetString(Patch, TEXT("target"));
    UObject* Object = LoadObject<UObject>(nullptr, *Target);
    if (!Object)
    {
        OutError = FString::Printf(TEXT("Could not load material target: %s"), *Target);
        return false;
    }

    UMaterial* Material = Cast<UMaterial>(Object);
    UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Object);
    if (!Material && !MIC)
    {
        OutError = TEXT("Patch target must be Material or MaterialInstanceConstant");
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
    if (!Patch->TryGetArrayField(TEXT("operations"), Ops) || !Ops)
    {
        OutError = TEXT("patch.operations array missing");
        return false;
    }

    const FScopedTransaction Transaction(NSLOCTEXT("AgentBlueprintTools", "PatchMaterial", "Patch Material"));
    Object->Modify();

    TMap<FString, UMaterialExpression*> ExpressionsById;
    TArray<FString> Messages;

    for (const TSharedPtr<FJsonValue>& V : *Ops)
    {
        TSharedPtr<FJsonObject> Op = V->AsObject();
        if (!Op.IsValid()) continue;
        const FString OpName = ABTJson::GetString(Op, TEXT("op"));

        if (OpName == TEXT("make_ui_circle"))
        {
            UMaterial* CircleMaterial = Material;
            if (!CircleMaterial && MIC)
            {
                CircleMaterial = Cast<UMaterial>(MIC->Parent);
            }

            if (!BuildUICircleMaterial(CircleMaterial, Op, Messages, OutError))
            {
                return false;
            }

            if (MIC)
            {
                MIC->Modify();
                UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(
                    MIC,
                    *ABTJson::GetString(Op, TEXT("colorParameter"), TEXT("CircleColor")),
                    ReadLinearColor(Op, FLinearColor::White));
            }
        }
        else if (OpName == TEXT("add_expression"))
        {
            if (!Material) { OutError = TEXT("add_expression requires UMaterial"); return false; }
            const FString Id = ABTJson::GetString(Op, TEXT("id"));
            UClass* ExprClass = ResolveExpressionClass(ABTJson::GetString(Op, TEXT("class")));
            if (!ExprClass) { OutError = TEXT("expression class not found"); return false; }

            UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpression(
                Material,
                ExprClass,
                ABTJson::GetInt(Op, TEXT("x"), 0),
                ABTJson::GetInt(Op, TEXT("y"), 0));

            if (!Expr) { OutError = TEXT("CreateMaterialExpression failed"); return false; }

            if (UMaterialExpressionVectorParameter* Vector = Cast<UMaterialExpressionVectorParameter>(Expr))
            {
                const FString ParamName = ABTJson::GetString(Op, TEXT("parameter_name"));
                if (!ParamName.IsEmpty()) Vector->ParameterName = *ParamName;
                Vector->DefaultValue = ReadLinearColor(Op, Vector->DefaultValue);
            }

            if (UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Expr))
            {
                const FString ParamName = ABTJson::GetString(Op, TEXT("parameter_name"));
                if (!ParamName.IsEmpty()) Scalar->ParameterName = *ParamName;
                Scalar->DefaultValue = static_cast<float>(ABTJson::GetNumber(Op, TEXT("value"), Scalar->DefaultValue));
            }

            if (!Id.IsEmpty()) ExpressionsById.Add(Id, Expr);
            Messages.Add(FString::Printf(TEXT("Added expression %s"), *Id));
        }
        else if (OpName == TEXT("connect"))
        {
            FString FromId, FromOutput, ToId, ToInput;
            SplitExpressionPin(ABTJson::GetString(Op, TEXT("from")), FromId, FromOutput);
            SplitExpressionPin(ABTJson::GetString(Op, TEXT("to")), ToId, ToInput);
            UMaterialExpression* From = ExpressionsById.FindRef(FromId);
            UMaterialExpression* To = ExpressionsById.FindRef(ToId);
            if (!From || !To) { OutError = TEXT("connect references unknown expression id"); return false; }
            if (!UMaterialEditingLibrary::ConnectMaterialExpressions(From, FromOutput, To, ToInput))
            {
                OutError = TEXT("ConnectMaterialExpressions failed"); return false;
            }
        }
        else if (OpName == TEXT("connect_material_property"))
        {
            FString FromId, FromOutput;
            SplitExpressionPin(ABTJson::GetString(Op, TEXT("from")), FromId, FromOutput);
            UMaterialExpression* From = ExpressionsById.FindRef(FromId);
            if (!From) { OutError = TEXT("connect_material_property references unknown expression id"); return false; }
            if (!UMaterialEditingLibrary::ConnectMaterialProperty(From, FromOutput, ResolveMaterialProperty(ABTJson::GetString(Op, TEXT("property")))))
            {
                OutError = TEXT("ConnectMaterialProperty failed"); return false;
            }
        }
        else if (OpName == TEXT("set_scalar_parameter"))
        {
            if (!MIC) { OutError = TEXT("set_scalar_parameter requires MaterialInstanceConstant"); return false; }
            UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MIC, *ABTJson::GetString(Op, TEXT("name")), ABTJson::GetNumber(Op, TEXT("value"), 0.0));
        }
        else if (OpName == TEXT("set_vector_parameter"))
        {
            if (!MIC) { OutError = TEXT("set_vector_parameter requires MaterialInstanceConstant"); return false; }
            UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(MIC, *ABTJson::GetString(Op, TEXT("name")), ReadLinearColor(Op, FLinearColor::White));
        }
        else
        {
            OutError = FString::Printf(TEXT("Unsupported material op: %s"), *OpName);
            return false;
        }
    }

    if (Material)
    {
        UMaterialEditingLibrary::RecompileMaterial(Material);
    }

    if (MIC)
    {
        MIC->PostEditChange();
    }

    if (bSaveOnSuccess)
    {
        UEditorAssetLibrary::SaveLoadedAsset(Object, false);
        if (MIC)
        {
            if (UMaterial* ParentMaterial = Cast<UMaterial>(MIC->Parent))
            {
                UEditorAssetLibrary::SaveLoadedAsset(ParentMaterial, false);
            }
        }
    }

    OutJson = ABTJson::Ok();
    TArray<TSharedPtr<FJsonValue>> JsonMessages;
    for (const FString& Message : Messages) JsonMessages.Add(ABTJson::StringValue(Message));
    OutJson->SetArrayField(TEXT("messages"), JsonMessages);
    OutJson->SetBoolField(TEXT("saved"), bSaveOnSuccess);
    return true;
}
