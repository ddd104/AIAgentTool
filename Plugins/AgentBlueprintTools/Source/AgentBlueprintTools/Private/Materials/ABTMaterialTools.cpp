#include "Materials/ABTMaterialTools.h"
#include "Utils/ABTJson.h"

#include "EditorAssetLibrary.h"
#include "MaterialEditingLibrary.h"
#include "MaterialDomain.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstantBiasScale.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSphereMask.h"
#include "Materials/MaterialExpressionSine.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "MaterialExpressionIO.h"
#include "MaterialShaderType.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    struct FMaterialRootPropertyRef
    {
        EMaterialProperty Property;
        const TCHAR* Name;
    };

    static const FMaterialRootPropertyRef GRootProperties[] =
    {
        { MP_BaseColor, TEXT("BaseColor") },
        { MP_Metallic, TEXT("Metallic") },
        { MP_Specular, TEXT("Specular") },
        { MP_Roughness, TEXT("Roughness") },
        { MP_EmissiveColor, TEXT("EmissiveColor") },
        { MP_Opacity, TEXT("Opacity") },
        { MP_OpacityMask, TEXT("OpacityMask") },
        { MP_Normal, TEXT("Normal") },
        { MP_WorldPositionOffset, TEXT("WorldPositionOffset") },
        { MP_PixelDepthOffset, TEXT("PixelDepthOffset") },
        { MP_AmbientOcclusion, TEXT("AmbientOcclusion") },
        { MP_Refraction, TEXT("Refraction") }
    };

    EMaterialProperty ResolveMaterialProperty(const FString& Name)
    {
        if (Name.Equals(TEXT("BaseColor"), ESearchCase::IgnoreCase)) return MP_BaseColor;
        if (Name.Equals(TEXT("Metallic"), ESearchCase::IgnoreCase)) return MP_Metallic;
        if (Name.Equals(TEXT("Specular"), ESearchCase::IgnoreCase)) return MP_Specular;
        if (Name.Equals(TEXT("Roughness"), ESearchCase::IgnoreCase)) return MP_Roughness;
        if (Name.Equals(TEXT("EmissiveColor"), ESearchCase::IgnoreCase)) return MP_EmissiveColor;
        if (Name.Equals(TEXT("Opacity"), ESearchCase::IgnoreCase)) return MP_Opacity;
        if (Name.Equals(TEXT("OpacityMask"), ESearchCase::IgnoreCase)) return MP_OpacityMask;
        if (Name.Equals(TEXT("Normal"), ESearchCase::IgnoreCase)) return MP_Normal;
        if (Name.Equals(TEXT("WorldPositionOffset"), ESearchCase::IgnoreCase)) return MP_WorldPositionOffset;
        if (Name.Equals(TEXT("PixelDepthOffset"), ESearchCase::IgnoreCase)) return MP_PixelDepthOffset;
        if (Name.Equals(TEXT("AmbientOcclusion"), ESearchCase::IgnoreCase)) return MP_AmbientOcclusion;
        if (Name.Equals(TEXT("Refraction"), ESearchCase::IgnoreCase)) return MP_Refraction;
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

    bool ParseBlendMode(const FString& Value, EBlendMode& OutBlendMode)
    {
        if (Value.Equals(TEXT("Opaque"), ESearchCase::IgnoreCase)) { OutBlendMode = BLEND_Opaque; return true; }
        if (Value.Equals(TEXT("Masked"), ESearchCase::IgnoreCase)) { OutBlendMode = BLEND_Masked; return true; }
        if (Value.Equals(TEXT("Translucent"), ESearchCase::IgnoreCase)) { OutBlendMode = BLEND_Translucent; return true; }
        if (Value.Equals(TEXT("Additive"), ESearchCase::IgnoreCase)) { OutBlendMode = BLEND_Additive; return true; }
        if (Value.Equals(TEXT("Modulate"), ESearchCase::IgnoreCase)) { OutBlendMode = BLEND_Modulate; return true; }
        if (Value.Equals(TEXT("AlphaComposite"), ESearchCase::IgnoreCase)) { OutBlendMode = BLEND_AlphaComposite; return true; }
        if (Value.Equals(TEXT("AlphaHoldout"), ESearchCase::IgnoreCase)) { OutBlendMode = BLEND_AlphaHoldout; return true; }
        return false;
    }

    bool ParseShadingModel(const FString& Value, EMaterialShadingModel& OutShadingModel)
    {
        if (Value.Equals(TEXT("Unlit"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("MSM_Unlit"), ESearchCase::IgnoreCase))
        {
            OutShadingModel = MSM_Unlit;
            return true;
        }
        if (Value.Equals(TEXT("DefaultLit"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("MSM_DefaultLit"), ESearchCase::IgnoreCase))
        {
            OutShadingModel = MSM_DefaultLit;
            return true;
        }
        return false;
    }

    bool ReadBoolMaterialProperty(const UMaterial* Material, const TCHAR* PropertyName, bool& OutValue)
    {
        if (!Material)
        {
            return false;
        }

        const FBoolProperty* Property = FindFProperty<FBoolProperty>(Material->GetClass(), PropertyName);
        if (!Property)
        {
            return false;
        }

        OutValue = Property->GetPropertyValue_InContainer(Material);
        return true;
    }

    bool SetBoolMaterialProperty(UMaterial* Material, const TCHAR* PropertyName, bool Value)
    {
        if (!Material)
        {
            return false;
        }

        FBoolProperty* Property = FindFProperty<FBoolProperty>(Material->GetClass(), PropertyName);
        if (!Property)
        {
            return false;
        }

        Property->SetPropertyValue_InContainer(Material, Value);
        return true;
    }

    void ApplyOptionalBoolSetting(
        UMaterial* Material,
        const TSharedPtr<FJsonObject>& Op,
        const TCHAR* FieldName,
        const TCHAR* PropertyName,
        const TCHAR* MessageName,
        TArray<FString>& OutMessages)
    {
        if (!Op->HasField(FieldName))
        {
            return;
        }

        bool CurrentValue = false;
        if (!ReadBoolMaterialProperty(Material, PropertyName, CurrentValue))
        {
            OutMessages.Add(FString::Printf(TEXT("Skipped %s; property %s is unavailable in this engine version"), MessageName, PropertyName));
            return;
        }

        const bool NewValue = ABTJson::GetBool(Op, FieldName, CurrentValue);
        if (SetBoolMaterialProperty(Material, PropertyName, NewValue))
        {
            OutMessages.Add(FString::Printf(TEXT("Set %s = %s"), MessageName, NewValue ? TEXT("true") : TEXT("false")));
        }
    }

    void ExportOptionalBoolSetting(UMaterial* Material, TSharedPtr<FJsonObject>& Json, const TCHAR* FieldName, const TCHAR* PropertyName)
    {
        bool Value = false;
        if (ReadBoolMaterialProperty(Material, PropertyName, Value))
        {
            Json->SetBoolField(FieldName, Value);
        }
    }

    void GatherMaterialExpressionSubobjects(UMaterial* Material, TArray<UMaterialExpression*>& OutExpressions, TArray<UMaterialExpressionComment*>* OutComments = nullptr)
    {
        if (!Material)
        {
            return;
        }

        TArray<UObject*> Subobjects;
        GetObjectsWithOuter(Material, Subobjects, true);

        TSet<UMaterialExpression*> SeenExpressions;
        for (UMaterialExpression* Expression : OutExpressions)
        {
            SeenExpressions.Add(Expression);
        }

        TSet<UMaterialExpressionComment*> SeenComments;
        if (OutComments)
        {
            for (UMaterialExpressionComment* Comment : *OutComments)
            {
                SeenComments.Add(Comment);
            }
        }

        for (UObject* Subobject : Subobjects)
        {
            if (!Subobject)
            {
                continue;
            }

            if (UMaterialExpressionComment* Comment = Cast<UMaterialExpressionComment>(Subobject))
            {
                if (OutComments && !SeenComments.Contains(Comment))
                {
                    OutComments->Add(Comment);
                    SeenComments.Add(Comment);
                }
                continue;
            }

            if (UMaterialExpression* Expression = Cast<UMaterialExpression>(Subobject))
            {
                if (!SeenExpressions.Contains(Expression))
                {
                    OutExpressions.Add(Expression);
                    SeenExpressions.Add(Expression);
                }
            }
        }
    }

    void GatherMaterialExpressions(UMaterial* Material, TArray<UMaterialExpression*>& OutExpressions)
    {
        if (!Material)
        {
            return;
        }

        for (UMaterialExpression* Expression : Material->GetExpressions())
        {
            if (Expression)
            {
                OutExpressions.Add(Expression);
            }
        }

        if (OutExpressions.IsEmpty())
        {
            GatherMaterialExpressionSubobjects(Material, OutExpressions);
        }
    }

    UMaterialExpression* FindMaterialExpressionByName(UMaterial* Material, const FString& Name)
    {
        if (!Material || Name.IsEmpty())
        {
            return nullptr;
        }

        TArray<UMaterialExpression*> Expressions;
        GatherMaterialExpressions(Material, Expressions);
        for (UMaterialExpression* Expression : Expressions)
        {
            if (!Expression)
            {
                continue;
            }

            if (Expression->GetName().Equals(Name, ESearchCase::IgnoreCase))
            {
                return Expression;
            }
        }

        return nullptr;
    }

    UMaterialExpression* ResolveMaterialExpressionRef(UMaterial* Material, const TMap<FString, UMaterialExpression*>& CreatedExpressions, const FString& Ref)
    {
        if (UMaterialExpression* const* Created = CreatedExpressions.Find(Ref))
        {
            return *Created;
        }

        return FindMaterialExpressionByName(Material, Ref);
    }

    FString OutputNameForExpression(UMaterialExpression* Expression, int32 OutputIndex)
    {
        if (!Expression)
        {
            return TEXT("");
        }

        TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();
        if (!Outputs.IsValidIndex(OutputIndex))
        {
            return TEXT("");
        }

        return Outputs[OutputIndex].OutputName.ToString();
    }

    int32 ResolveExpressionOutputIndex(UMaterialExpression* Expression, const FString& OutputRef)
    {
        if (!Expression)
        {
            return INDEX_NONE;
        }

        if (OutputRef.IsEmpty())
        {
            return 0;
        }

        if (OutputRef.IsNumeric())
        {
            return FCString::Atoi(*OutputRef);
        }

        TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();
        for (int32 Index = 0; Index < Outputs.Num(); ++Index)
        {
            if (Outputs[Index].OutputName.ToString().Equals(OutputRef, ESearchCase::IgnoreCase))
            {
                return Index;
            }
        }

        return INDEX_NONE;
    }

    void ExportExpressionInput(const FString& InputName, const FExpressionInput* Input, TArray<TSharedPtr<FJsonValue>>& OutInputs)
    {
        if (!Input || !Input->Expression)
        {
            return;
        }

        TSharedPtr<FJsonObject> InputJson = ABTJson::Object();
        InputJson->SetStringField(TEXT("name"), InputName);
        InputJson->SetStringField(TEXT("source"), Input->Expression->GetName());
        InputJson->SetStringField(TEXT("source_class"), Input->Expression->GetClass()->GetPathName());
        InputJson->SetNumberField(TEXT("output_index"), Input->OutputIndex);
        InputJson->SetStringField(TEXT("output_name"), OutputNameForExpression(Input->Expression, Input->OutputIndex));
        InputJson->SetStringField(TEXT("input_name"), Input->InputName.ToString());
        InputJson->SetNumberField(TEXT("mask"), Input->Mask);
        InputJson->SetNumberField(TEXT("mask_r"), Input->MaskR);
        InputJson->SetNumberField(TEXT("mask_g"), Input->MaskG);
        InputJson->SetNumberField(TEXT("mask_b"), Input->MaskB);
        InputJson->SetNumberField(TEXT("mask_a"), Input->MaskA);
        OutInputs.Add(ABTJson::ObjectValue(InputJson));
    }

    void ExportRootInputs(UMaterial* Material, TSharedPtr<FJsonObject>& OutJson)
    {
        TArray<TSharedPtr<FJsonValue>> RootInputs;
        if (!Material)
        {
            OutJson->SetArrayField(TEXT("material_outputs"), RootInputs);
            return;
        }

        for (const FMaterialRootPropertyRef& Root : GRootProperties)
        {
            FExpressionInput* Input = Material->GetExpressionInputForProperty(Root.Property);
            if (!Input || !Input->Expression)
            {
                continue;
            }

            TArray<TSharedPtr<FJsonValue>> Inputs;
            ExportExpressionInput(Root.Name, Input, Inputs);
            if (Inputs.Num() > 0)
            {
                RootInputs.Add(Inputs[0]);
            }
        }

        OutJson->SetArrayField(TEXT("material_outputs"), RootInputs);
    }

    void GatherReachableExpression(UMaterialExpression* Expression, TSet<UMaterialExpression*>& Reachable)
    {
        if (!Expression || Reachable.Contains(Expression))
        {
            return;
        }

        Reachable.Add(Expression);

        const int32 InputCount = Expression->GetInputsView().Num();
        for (int32 InputIndex = 0; InputIndex < InputCount; ++InputIndex)
        {
            FExpressionInput* Input = Expression->GetInput(InputIndex);
            if (Input && Input->Expression)
            {
                GatherReachableExpression(Input->Expression, Reachable);
            }
        }
    }

    void GatherReachableExpressions(UMaterial* Material, TSet<UMaterialExpression*>& OutReachable)
    {
        if (!Material)
        {
            return;
        }

        for (const FMaterialRootPropertyRef& Root : GRootProperties)
        {
            FExpressionInput* Input = Material->GetExpressionInputForProperty(Root.Property);
            if (Input && Input->Expression)
            {
                GatherReachableExpression(Input->Expression, OutReachable);
            }
        }
    }

    void ExportReachabilityStats(UMaterial* Material, TSharedPtr<FJsonObject>& Json)
    {
        TArray<UMaterialExpression*> Expressions;
        GatherMaterialExpressions(Material, Expressions);

        TSet<UMaterialExpression*> Reachable;
        GatherReachableExpressions(Material, Reachable);

        int32 ReachableTextureSamples = 0;
        int32 UnreachableTextureSamples = 0;
        for (UMaterialExpression* Expression : Expressions)
        {
            if (!Expression || !Cast<UMaterialExpressionTextureSample>(Expression))
            {
                continue;
            }

            if (Reachable.Contains(Expression))
            {
                ++ReachableTextureSamples;
            }
            else
            {
                ++UnreachableTextureSamples;
            }
        }

        Json->SetNumberField(TEXT("reachable_expression_count"), Reachable.Num());
        Json->SetNumberField(TEXT("unreachable_expression_count"), Expressions.Num() - Reachable.Num());
        Json->SetNumberField(TEXT("reachable_texture_sample_count"), ReachableTextureSamples);
        Json->SetNumberField(TEXT("unreachable_texture_sample_count"), UnreachableTextureSamples);
    }

    TSharedPtr<FJsonValue> JsonNumber(double Value)
    {
        return MakeShared<FJsonValueNumber>(Value);
    }

    TSharedPtr<FJsonValue> JsonBool(bool Value)
    {
        return MakeShared<FJsonValueBoolean>(Value);
    }

    void ExportLinearColor(const FLinearColor& Color, TSharedPtr<FJsonObject>& Json, const FString& FieldName)
    {
        TArray<TSharedPtr<FJsonValue>> Value;
        Value.Add(JsonNumber(Color.R));
        Value.Add(JsonNumber(Color.G));
        Value.Add(JsonNumber(Color.B));
        Value.Add(JsonNumber(Color.A));
        Json->SetArrayField(FieldName, Value);
    }

    void ExportExpressionParameters(UMaterialExpression* Expression, TSharedPtr<FJsonObject>& Json)
    {
        if (!Expression || !Json.IsValid())
        {
            return;
        }

        if (UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression))
        {
            Json->SetStringField(TEXT("parameter_name"), Scalar->ParameterName.ToString());
            Json->SetNumberField(TEXT("default_value"), Scalar->DefaultValue);
        }

        if (UMaterialExpressionVectorParameter* Vector = Cast<UMaterialExpressionVectorParameter>(Expression))
        {
            Json->SetStringField(TEXT("parameter_name"), Vector->ParameterName.ToString());
            ExportLinearColor(Vector->DefaultValue, Json, TEXT("default_value"));
        }

        if (UMaterialExpressionTextureSample* TextureSample = Cast<UMaterialExpressionTextureSample>(Expression))
        {
            Json->SetStringField(TEXT("texture"), TextureSample->Texture ? TextureSample->Texture->GetPathName() : TEXT(""));
            Json->SetNumberField(TEXT("sampler_type"), static_cast<int32>(TextureSample->SamplerType));
            Json->SetNumberField(TEXT("sampler_source"), static_cast<int32>(TextureSample->SamplerSource));
            Json->SetNumberField(TEXT("mip_value_mode"), static_cast<int32>(TextureSample->MipValueMode));
        }

        if (UMaterialExpressionTextureCoordinate* TextureCoordinate = Cast<UMaterialExpressionTextureCoordinate>(Expression))
        {
            Json->SetNumberField(TEXT("coordinate_index"), TextureCoordinate->CoordinateIndex);
            Json->SetNumberField(TEXT("u_tiling"), TextureCoordinate->UTiling);
            Json->SetNumberField(TEXT("v_tiling"), TextureCoordinate->VTiling);
        }

        for (TFieldIterator<FProperty> It(Expression->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DisableEditOnInstance))
            {
                continue;
            }

            const FString PropertyName = Property->GetName();
            if (PropertyName == TEXT("MaterialExpressionEditorX") ||
                PropertyName == TEXT("MaterialExpressionEditorY") ||
                PropertyName == TEXT("GraphNode") ||
                PropertyName == TEXT("Outputs") ||
                PropertyName == TEXT("Desc") ||
                PropertyName == TEXT("bCommentBubbleVisible") ||
                PropertyName == TEXT("bHidePreviewWindow") ||
                PropertyName == TEXT("bCollapsed") ||
                PropertyName == TEXT("bShaderInputData") ||
                PropertyName == TEXT("Material"))
            {
                continue;
            }

            if (Json->HasField(PropertyName))
            {
                continue;
            }

            if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
            {
                Json->SetBoolField(PropertyName, BoolProp->GetPropertyValue_InContainer(Expression));
            }
            else if (const FNumericProperty* NumberProp = CastField<FNumericProperty>(Property))
            {
                if (NumberProp->IsInteger())
                {
                    Json->SetNumberField(PropertyName, static_cast<double>(NumberProp->GetSignedIntPropertyValue(NumberProp->ContainerPtrToValuePtr<void>(Expression))));
                }
                else
                {
                    Json->SetNumberField(PropertyName, NumberProp->GetFloatingPointPropertyValue(NumberProp->ContainerPtrToValuePtr<void>(Expression)));
                }
            }
            else if (const FNameProperty* NameProp = CastField<FNameProperty>(Property))
            {
                Json->SetStringField(PropertyName, NameProp->GetPropertyValue_InContainer(Expression).ToString());
            }
            else if (const FStrProperty* StrProp = CastField<FStrProperty>(Property))
            {
                Json->SetStringField(PropertyName, StrProp->GetPropertyValue_InContainer(Expression));
            }
            else if (const FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Property))
            {
                UObject* ObjectValue = ObjectProp->GetObjectPropertyValue_InContainer(Expression);
                if (ObjectValue)
                {
                    Json->SetStringField(PropertyName, ObjectValue->GetPathName());
                }
            }
            else if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
            {
                if (StructProp->Struct && StructProp->Struct->GetFName() == NAME_LinearColor)
                {
                    const FLinearColor* Color = StructProp->ContainerPtrToValuePtr<FLinearColor>(Expression);
                    if (Color)
                    {
                        ExportLinearColor(*Color, Json, PropertyName);
                    }
                }
            }
        }
    }

    void ExportExpressionDetails(UMaterialExpression* Expression, TSharedPtr<FJsonObject>& Json)
    {
        if (!Expression || !Json.IsValid())
        {
            return;
        }

        TArray<TSharedPtr<FJsonValue>> Inputs;
        const int32 InputCount = Expression->GetInputsView().Num();
        for (int32 InputIndex = 0; InputIndex < InputCount; ++InputIndex)
        {
            FExpressionInput* Input = Expression->GetInput(InputIndex);
            const FString InputName = Expression->GetInputName(InputIndex).ToString();
            ExportExpressionInput(InputName.IsEmpty() ? FString::Printf(TEXT("Input%d"), InputIndex) : InputName, Input, Inputs);
        }
        Json->SetArrayField(TEXT("inputs"), Inputs);

        TArray<TSharedPtr<FJsonValue>> Outputs;
        TArray<FExpressionOutput>& ExpressionOutputs = Expression->GetOutputs();
        for (int32 OutputIndex = 0; OutputIndex < ExpressionOutputs.Num(); ++OutputIndex)
        {
            TSharedPtr<FJsonObject> OutputJson = ABTJson::Object();
            OutputJson->SetNumberField(TEXT("index"), OutputIndex);
            OutputJson->SetStringField(TEXT("name"), ExpressionOutputs[OutputIndex].OutputName.ToString());
            Outputs.Add(ABTJson::ObjectValue(OutputJson));
        }
        Json->SetArrayField(TEXT("outputs"), Outputs);

        TSharedPtr<FJsonObject> Parameters = ABTJson::Object();
        ExportExpressionParameters(Expression, Parameters);
        Json->SetObjectField(TEXT("parameters"), Parameters);
    }

    bool SetLinearColorFromJson(FStructProperty* StructProperty, void* Container, const TSharedPtr<FJsonValue>& Value)
    {
        if (!StructProperty || !StructProperty->Struct || StructProperty->Struct->GetFName() != NAME_LinearColor || !Value.IsValid())
        {
            return false;
        }

        FLinearColor* Color = StructProperty->ContainerPtrToValuePtr<FLinearColor>(Container);
        if (!Color)
        {
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (Value->TryGetArray(Array) && Array && Array->Num() >= 3)
        {
            Color->R = static_cast<float>((*Array)[0]->AsNumber());
            Color->G = static_cast<float>((*Array)[1]->AsNumber());
            Color->B = static_cast<float>((*Array)[2]->AsNumber());
            Color->A = Array->Num() > 3 ? static_cast<float>((*Array)[3]->AsNumber()) : 1.0f;
            return true;
        }

        const TSharedPtr<FJsonObject>* Object = nullptr;
        if (Value->TryGetObject(Object) && Object && Object->IsValid())
        {
            Color->R = static_cast<float>((*Object)->GetNumberField(TEXT("r")));
            Color->G = static_cast<float>((*Object)->GetNumberField(TEXT("g")));
            Color->B = static_cast<float>((*Object)->GetNumberField(TEXT("b")));
            Color->A = (*Object)->HasField(TEXT("a")) ? static_cast<float>((*Object)->GetNumberField(TEXT("a"))) : 1.0f;
            return true;
        }

        return false;
    }

    bool SetSimpleObjectProperty(UObject* Object, const FString& PropertyName, const TSharedPtr<FJsonValue>& Value, FString& OutError)
    {
        if (!Object || PropertyName.IsEmpty() || !Value.IsValid())
        {
            OutError = TEXT("set_expression_property requires expression, property and value");
            return false;
        }

        FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), *PropertyName);
        if (!Property)
        {
            OutError = FString::Printf(TEXT("Property not found on %s: %s"), *Object->GetName(), *PropertyName);
            return false;
        }

        Object->Modify();

        if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
        {
            BoolProp->SetPropertyValue_InContainer(Object, Value->AsBool());
            return true;
        }

        if (FNumericProperty* NumberProp = CastField<FNumericProperty>(Property))
        {
            if (NumberProp->IsInteger())
            {
                NumberProp->SetIntPropertyValue(NumberProp->ContainerPtrToValuePtr<void>(Object), static_cast<int64>(Value->AsNumber()));
            }
            else
            {
                NumberProp->SetFloatingPointPropertyValue(NumberProp->ContainerPtrToValuePtr<void>(Object), Value->AsNumber());
            }
            return true;
        }

        if (FNameProperty* NameProp = CastField<FNameProperty>(Property))
        {
            NameProp->SetPropertyValue_InContainer(Object, FName(*Value->AsString()));
            return true;
        }

        if (FStrProperty* StrProp = CastField<FStrProperty>(Property))
        {
            StrProp->SetPropertyValue_InContainer(Object, Value->AsString());
            return true;
        }

        if (FTextProperty* TextProp = CastField<FTextProperty>(Property))
        {
            TextProp->SetPropertyValue_InContainer(Object, FText::FromString(Value->AsString()));
            return true;
        }

        if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
        {
            int64 IntValue = 0;
            if (Value->Type == EJson::String)
            {
                IntValue = EnumProp->GetEnum()->GetValueByNameString(Value->AsString());
                if (IntValue == INDEX_NONE)
                {
                    OutError = FString::Printf(TEXT("Unknown enum value %s for %s"), *Value->AsString(), *PropertyName);
                    return false;
                }
            }
            else
            {
                IntValue = static_cast<int64>(Value->AsNumber());
            }
            EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(EnumProp->ContainerPtrToValuePtr<void>(Object), IntValue);
            return true;
        }

        if (FByteProperty* ByteProp = CastField<FByteProperty>(Property))
        {
            if (ByteProp->Enum && Value->Type == EJson::String)
            {
                const int64 EnumValue = ByteProp->Enum->GetValueByNameString(Value->AsString());
                if (EnumValue == INDEX_NONE)
                {
                    OutError = FString::Printf(TEXT("Unknown byte enum value %s for %s"), *Value->AsString(), *PropertyName);
                    return false;
                }
                ByteProp->SetIntPropertyValue(ByteProp->ContainerPtrToValuePtr<void>(Object), EnumValue);
            }
            else
            {
                ByteProp->SetIntPropertyValue(ByteProp->ContainerPtrToValuePtr<void>(Object), static_cast<int64>(Value->AsNumber()));
            }
            return true;
        }

        if (FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Property))
        {
            UObject* LoadedObject = Value->AsString().IsEmpty() ? nullptr : LoadObject<UObject>(nullptr, *Value->AsString());
            if (!LoadedObject && !Value->AsString().IsEmpty())
            {
                OutError = FString::Printf(TEXT("Could not load object for %s: %s"), *PropertyName, *Value->AsString());
                return false;
            }
            ObjectProp->SetObjectPropertyValue_InContainer(Object, LoadedObject);
            return true;
        }

        if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
        {
            if (SetLinearColorFromJson(StructProp, Object, Value))
            {
                return true;
            }
        }

        OutError = FString::Printf(TEXT("Unsupported property type for %s: %s"), *PropertyName, *Property->GetClass()->GetName());
        return false;
    }

    bool ReplaceMaterialExpressionReferences(UMaterial* Material, UMaterialExpression* From, UMaterialExpression* To, int32 ToOutputIndex, TArray<FString>& OutMessages)
    {
        if (!Material || !From)
        {
            return false;
        }

        int32 Replaced = 0;

        for (const FMaterialRootPropertyRef& Root : GRootProperties)
        {
            FExpressionInput* Input = Material->GetExpressionInputForProperty(Root.Property);
            if (Input && Input->Expression == From)
            {
                Material->Modify();
                if (To)
                {
                    Input->Connect(ToOutputIndex, To);
                }
                else
                {
                    Input->Expression = nullptr;
                    Input->OutputIndex = 0;
                }
                Material->MarkPackageDirty();
                ++Replaced;
            }
        }

        TArray<UMaterialExpression*> Expressions;
        GatherMaterialExpressions(Material, Expressions);
        for (UMaterialExpression* Expression : Expressions)
        {
            if (!Expression || Expression == From)
            {
                continue;
            }

            const int32 InputCount = Expression->GetInputsView().Num();
            for (int32 InputIndex = 0; InputIndex < InputCount; ++InputIndex)
            {
                FExpressionInput* Input = Expression->GetInput(InputIndex);
                if (Input && Input->Expression == From)
                {
                    Expression->Modify();
                    if (To)
                    {
                        Input->Connect(ToOutputIndex, To);
                    }
                    else
                    {
                        Input->Expression = nullptr;
                        Input->OutputIndex = 0;
                    }
                    Expression->PostEditChange();
                    Expression->MarkPackageDirty();
                    ++Replaced;
                }
            }
        }

        if (Replaced > 0)
        {
            Material->MarkPackageDirty();
        }

        OutMessages.Add(FString::Printf(
            TEXT("Replaced %d references from %s to %s"),
            Replaced,
            *From->GetName(),
            To ? *To->GetName() : TEXT("<disconnected>")));
        return Replaced > 0;
    }

    bool RepairMaterialExpressionCollection(UMaterial* Material, TArray<FString>& OutMessages, FString& OutError)
    {
        if (!Material)
        {
            OutError = TEXT("repair_expression_collection requires UMaterial");
            return false;
        }

        TArray<UMaterialExpression*> Expressions;
        TArray<UMaterialExpressionComment*> Comments;
        GatherMaterialExpressionSubobjects(Material, Expressions, &Comments);
        if (Expressions.IsEmpty() && Comments.IsEmpty())
        {
            OutMessages.Add(TEXT("No material expression subobjects found to repair"));
            return true;
        }

        Material->Modify();
        Material->PreEditChange(nullptr);

        FMaterialExpressionCollection& Collection = Material->GetExpressionCollection();

        int32 AddedExpressions = 0;
        for (UMaterialExpression* Expression : Expressions)
        {
            if (!Expression)
            {
                continue;
            }

            if (!Collection.Expressions.Contains(Expression))
            {
                Collection.AddExpression(Expression);
                ++AddedExpressions;
            }
        }

        int32 AddedComments = 0;
        for (UMaterialExpressionComment* Comment : Comments)
        {
            if (!Comment)
            {
                continue;
            }

            if (!Collection.EditorComments.Contains(Comment))
            {
                Collection.AddComment(Comment);
                ++AddedComments;
            }
        }

        Material->PostEditChange();
        UMaterialEditingLibrary::RecompileMaterial(Material);

        OutMessages.Add(FString::Printf(
            TEXT("Repaired expression collection: added %d expressions and %d comments"),
            AddedExpressions,
            AddedComments));
        return true;
    }

    bool ApplyMaterialSettings(UMaterial* Material, const TSharedPtr<FJsonObject>& Op, TArray<FString>& OutMessages, FString& OutError)
    {
        if (!Material)
        {
            OutError = TEXT("set_material_settings requires UMaterial");
            return false;
        }

        Material->Modify();
        Material->PreEditChange(nullptr);

        const FString BlendModeValue = ABTJson::GetString(Op, TEXT("blendMode"));
        if (!BlendModeValue.IsEmpty())
        {
            EBlendMode BlendMode = BLEND_Opaque;
            if (!ParseBlendMode(BlendModeValue, BlendMode))
            {
                OutError = FString::Printf(TEXT("Unsupported blendMode: %s"), *BlendModeValue);
                return false;
            }
            Material->BlendMode = BlendMode;
            OutMessages.Add(FString::Printf(TEXT("Set blendMode = %s"), *BlendModeValue));
        }

        const FString ShadingModelValue = ABTJson::GetString(Op, TEXT("shadingModel"));
        if (!ShadingModelValue.IsEmpty())
        {
            EMaterialShadingModel ShadingModel = MSM_DefaultLit;
            if (!ParseShadingModel(ShadingModelValue, ShadingModel))
            {
                OutError = FString::Printf(TEXT("Unsupported shadingModel: %s"), *ShadingModelValue);
                return false;
            }
            Material->SetShadingModel(ShadingModel);
            OutMessages.Add(FString::Printf(TEXT("Set shadingModel = %s"), *ShadingModelValue));
        }

        if (Op->HasField(TEXT("twoSided")))
        {
            Material->TwoSided = ABTJson::GetBool(Op, TEXT("twoSided"), Material->TwoSided);
            OutMessages.Add(FString::Printf(TEXT("Set twoSided = %s"), Material->TwoSided ? TEXT("true") : TEXT("false")));
        }

        if (Op->HasField(TEXT("fullyRough")))
        {
            Material->bFullyRough = ABTJson::GetBool(Op, TEXT("fullyRough"), Material->bFullyRough);
            OutMessages.Add(FString::Printf(TEXT("Set fullyRough = %s"), Material->bFullyRough ? TEXT("true") : TEXT("false")));
        }

        ApplyOptionalBoolSetting(Material, Op, TEXT("separateTranslucency"), TEXT("bEnableSeparateTranslucency"), TEXT("separateTranslucency"), OutMessages);
        ApplyOptionalBoolSetting(Material, Op, TEXT("responsiveAA"), TEXT("bEnableResponsiveAA"), TEXT("responsiveAA"), OutMessages);
        ApplyOptionalBoolSetting(Material, Op, TEXT("screenSpaceReflections"), TEXT("bScreenSpaceReflections"), TEXT("screenSpaceReflections"), OutMessages);
        ApplyOptionalBoolSetting(Material, Op, TEXT("contactShadows"), TEXT("bContactShadows"), TEXT("contactShadows"), OutMessages);
        ApplyOptionalBoolSetting(Material, Op, TEXT("disableDepthTest"), TEXT("bDisableDepthTest"), TEXT("disableDepthTest"), OutMessages);
        ApplyOptionalBoolSetting(Material, Op, TEXT("translucencyVertexFog"), TEXT("bUseTranslucencyVertexFog"), TEXT("translucencyVertexFog"), OutMessages);
        ApplyOptionalBoolSetting(Material, Op, TEXT("computeFogPerPixel"), TEXT("bComputeFogPerPixel"), TEXT("computeFogPerPixel"), OutMessages);
        ApplyOptionalBoolSetting(Material, Op, TEXT("outputTranslucentVelocity"), TEXT("bOutputTranslucentVelocity"), TEXT("outputTranslucentVelocity"), OutMessages);
        ApplyOptionalBoolSetting(Material, Op, TEXT("allowNegativeEmissiveColor"), TEXT("bAllowNegativeEmissiveColor"), TEXT("allowNegativeEmissiveColor"), OutMessages);
        ApplyOptionalBoolSetting(Material, Op, TEXT("castRayTracedShadows"), TEXT("bCastRayTracedShadows"), TEXT("castRayTracedShadows"), OutMessages);

        Material->PostEditChange();
        return true;
    }

    void ExportMaterialStats(UMaterial* Material, TSharedPtr<FJsonObject>& Json)
    {
        if (!Material || !Json.IsValid())
        {
            return;
        }

        TArray<UMaterialExpression*> Expressions;
        GatherMaterialExpressions(Material, Expressions);

        int32 TextureSampleCount = 0;
        int32 CustomExpressionCount = 0;
        for (UMaterialExpression* Expr : Expressions)
        {
            if (Cast<UMaterialExpressionTextureSample>(Expr))
            {
                ++TextureSampleCount;
            }
            if (Cast<UMaterialExpressionCustom>(Expr))
            {
                ++CustomExpressionCount;
            }
        }

        Json->SetBoolField(TEXT("fully_rough"), Material->bFullyRough);
        Json->SetStringField(TEXT("shading_models"), GetShadingModelFieldString(Material->GetShadingModels()));
        Json->SetNumberField(TEXT("translucency_lighting_mode"), static_cast<int32>(Material->TranslucencyLightingMode));
        ExportOptionalBoolSetting(Material, Json, TEXT("separate_translucency"), TEXT("bEnableSeparateTranslucency"));
        ExportOptionalBoolSetting(Material, Json, TEXT("responsive_aa"), TEXT("bEnableResponsiveAA"));
        ExportOptionalBoolSetting(Material, Json, TEXT("screen_space_reflections"), TEXT("bScreenSpaceReflections"));
        ExportOptionalBoolSetting(Material, Json, TEXT("contact_shadows"), TEXT("bContactShadows"));
        ExportOptionalBoolSetting(Material, Json, TEXT("disable_depth_test"), TEXT("bDisableDepthTest"));
        ExportOptionalBoolSetting(Material, Json, TEXT("translucency_vertex_fog"), TEXT("bUseTranslucencyVertexFog"));
        ExportOptionalBoolSetting(Material, Json, TEXT("compute_fog_per_pixel"), TEXT("bComputeFogPerPixel"));
        ExportOptionalBoolSetting(Material, Json, TEXT("output_translucent_velocity"), TEXT("bOutputTranslucentVelocity"));
        ExportOptionalBoolSetting(Material, Json, TEXT("allow_negative_emissive_color"), TEXT("bAllowNegativeEmissiveColor"));
        ExportOptionalBoolSetting(Material, Json, TEXT("cast_ray_traced_shadows"), TEXT("bCastRayTracedShadows"));
        Json->SetNumberField(TEXT("expression_count"), Expressions.Num());
        Json->SetNumberField(TEXT("expression_collection_count"), Material->GetExpressions().Num());
        Json->SetNumberField(TEXT("texture_sample_count"), TextureSampleCount);
        Json->SetNumberField(TEXT("custom_expression_count"), CustomExpressionCount);
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

    bool BuildFlashSurfaceMaterial(UMaterial* Material, const TSharedPtr<FJsonObject>& Op, TArray<FString>& OutMessages, FString& OutError)
    {
        if (!Material)
        {
            OutError = TEXT("make_flash_surface requires a UMaterial or a material instance with a UMaterial parent");
            return false;
        }

        Material->Modify();
        UMaterialEditingLibrary::DeleteAllMaterialExpressions(Material);

        Material->MaterialDomain = MD_Surface;
        Material->BlendMode = BLEND_Opaque;
        Material->SetShadingModel(MSM_Unlit);
        Material->TwoSided = true;

        UMaterialExpressionTime* Time = CreateExpression<UMaterialExpressionTime>(Material, -980, 0);
        UMaterialExpressionSine* Sine = CreateExpression<UMaterialExpressionSine>(Material, -760, 0);
        UMaterialExpressionConstantBiasScale* Pulse = CreateExpression<UMaterialExpressionConstantBiasScale>(Material, -540, 0);
        UMaterialExpressionVectorParameter* Color = CreateExpression<UMaterialExpressionVectorParameter>(Material, -540, -180);
        UMaterialExpressionMultiply* ColorTimesPulse = CreateExpression<UMaterialExpressionMultiply>(Material, -280, -80);
        UMaterialExpressionMultiply* EmissiveBoost = CreateExpression<UMaterialExpressionMultiply>(Material, -60, -80);
        UMaterialExpressionConstant3Vector* BaseColor = CreateExpression<UMaterialExpressionConstant3Vector>(Material, -280, 130);

        if (!Time || !Sine || !Pulse || !Color || !ColorTimesPulse || !EmissiveBoost || !BaseColor)
        {
            OutError = TEXT("Failed to create one or more flash material expressions");
            return false;
        }

        Time->bIgnorePause = true;
        Sine->Period = static_cast<float>(ABTJson::GetNumber(Op, TEXT("period"), 1.0));
        Pulse->Bias = 1.0f;
        Pulse->Scale = 0.5f;
        Color->ParameterName = *ABTJson::GetString(Op, TEXT("colorParameter"), TEXT("FlashColor"));
        Color->DefaultValue = ReadLinearColor(Op, FLinearColor(0.2f, 0.65f, 1.0f, 1.0f));
        BaseColor->Constant = Color->DefaultValue;

        ColorTimesPulse->B.Connect(0, Pulse);
        ColorTimesPulse->A.Connect(0, Color);
        EmissiveBoost->A.Connect(0, ColorTimesPulse);
        EmissiveBoost->ConstB = static_cast<float>(ABTJson::GetNumber(Op, TEXT("intensity"), 8.0));
        Sine->Input.Connect(0, Time);
        Pulse->Input.Connect(0, Sine);

        if (!UMaterialEditingLibrary::ConnectMaterialProperty(BaseColor, TEXT(""), MP_BaseColor))
        {
            OutError = TEXT("Failed to connect flash material BaseColor");
            return false;
        }

        if (!UMaterialEditingLibrary::ConnectMaterialProperty(EmissiveBoost, TEXT(""), MP_EmissiveColor))
        {
            OutError = TEXT("Failed to connect flash material EmissiveColor");
            return false;
        }

        Material->PreEditChange(nullptr);
        Material->PostEditChange();
        UMaterialEditingLibrary::RecompileMaterial(Material);

        OutMessages.Add(TEXT("Configured material as flashing unlit surface"));
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
        ExportMaterialStats(Material, OutJson);
        ExportRootInputs(Material, OutJson);
        ExportReachabilityStats(Material, OutJson);

        TArray<TSharedPtr<FJsonValue>> Expressions;
        TArray<TSharedPtr<FJsonValue>> TextureSamples;
        TArray<TSharedPtr<FJsonValue>> FunctionCalls;
        TArray<UMaterialExpression*> MaterialExpressions;
        GatherMaterialExpressions(Material, MaterialExpressions);
        for (UMaterialExpression* Expr : MaterialExpressions)
        {
            if (!Expr) continue;
            TSharedPtr<FJsonObject> E = ABTJson::Object();
            E->SetStringField(TEXT("name"), Expr->GetName());
            E->SetStringField(TEXT("class"), Expr->GetClass()->GetPathName());
            E->SetNumberField(TEXT("x"), Expr->MaterialExpressionEditorX);
            E->SetNumberField(TEXT("y"), Expr->MaterialExpressionEditorY);
            ExportExpressionDetails(Expr, E);
            Expressions.Add(ABTJson::ObjectValue(E));

            if (Cast<UMaterialExpressionTextureSample>(Expr))
            {
                TextureSamples.Add(ABTJson::ObjectValue(E));
            }
            if (Expr->GetClass()->GetName().Contains(TEXT("MaterialFunctionCall")))
            {
                FunctionCalls.Add(ABTJson::ObjectValue(E));
            }
        }
        OutJson->SetArrayField(TEXT("expressions"), Expressions);
        OutJson->SetArrayField(TEXT("texture_samples"), TextureSamples);
        OutJson->SetArrayField(TEXT("function_calls"), FunctionCalls);
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
            Parent->SetBoolField(TEXT("two_sided"), ParentMaterial->TwoSided);
            ExportMaterialStats(ParentMaterial, Parent);
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
    if (Material)
    {
        Material->Modify();
    }

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
        else if (OpName == TEXT("make_flash_surface"))
        {
            UMaterial* FlashMaterial = Material;
            if (!FlashMaterial && MIC)
            {
                FlashMaterial = Cast<UMaterial>(MIC->Parent);
            }

            if (!BuildFlashSurfaceMaterial(FlashMaterial, Op, Messages, OutError))
            {
                return false;
            }

            if (MIC)
            {
                MIC->Modify();
                UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(
                    MIC,
                    *ABTJson::GetString(Op, TEXT("colorParameter"), TEXT("FlashColor")),
                    ReadLinearColor(Op, FLinearColor(0.2f, 0.65f, 1.0f, 1.0f)));
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
            UMaterialExpression* From = ResolveMaterialExpressionRef(Material, ExpressionsById, FromId);
            UMaterialExpression* To = ResolveMaterialExpressionRef(Material, ExpressionsById, ToId);
            if (!From || !To) { OutError = TEXT("connect references unknown expression id"); return false; }
            if (!UMaterialEditingLibrary::ConnectMaterialExpressions(From, FromOutput, To, ToInput))
            {
                OutError = TEXT("ConnectMaterialExpressions failed"); return false;
            }
            Messages.Add(FString::Printf(TEXT("Connected %s.%s to %s.%s"), *FromId, *FromOutput, *ToId, *ToInput));
        }
        else if (OpName == TEXT("connect_material_property"))
        {
            FString FromId, FromOutput;
            SplitExpressionPin(ABTJson::GetString(Op, TEXT("from")), FromId, FromOutput);
            UMaterialExpression* From = ResolveMaterialExpressionRef(Material, ExpressionsById, FromId);
            if (!From) { OutError = TEXT("connect_material_property references unknown expression id"); return false; }
            if (!UMaterialEditingLibrary::ConnectMaterialProperty(From, FromOutput, ResolveMaterialProperty(ABTJson::GetString(Op, TEXT("property")))))
            {
                OutError = TEXT("ConnectMaterialProperty failed"); return false;
            }
            Messages.Add(FString::Printf(TEXT("Connected %s.%s to material %s"), *FromId, *FromOutput, *ABTJson::GetString(Op, TEXT("property"))));
        }
        else if (OpName == TEXT("set_expression_property"))
        {
            if (!Material) { OutError = TEXT("set_expression_property requires UMaterial"); return false; }
            UMaterialExpression* Expression = ResolveMaterialExpressionRef(Material, ExpressionsById, ABTJson::GetString(Op, TEXT("expression")));
            if (!Expression)
            {
                OutError = FString::Printf(TEXT("set_expression_property referenced unknown expression: %s"), *ABTJson::GetString(Op, TEXT("expression")));
                return false;
            }

            const TSharedPtr<FJsonValue>* ValuePtr = Op->Values.Find(TEXT("value"));
            if (!ValuePtr || !ValuePtr->IsValid())
            {
                OutError = TEXT("set_expression_property.value missing");
                return false;
            }

            if (!SetSimpleObjectProperty(Expression, ABTJson::GetString(Op, TEXT("property")), *ValuePtr, OutError))
            {
                return false;
            }

            Expression->PostEditChange();
            Messages.Add(FString::Printf(
                TEXT("Set %s.%s"),
                *Expression->GetName(),
                *ABTJson::GetString(Op, TEXT("property"))));
        }
        else if (OpName == TEXT("replace_expression_references"))
        {
            if (!Material) { OutError = TEXT("replace_expression_references requires UMaterial"); return false; }
            UMaterialExpression* From = ResolveMaterialExpressionRef(Material, ExpressionsById, ABTJson::GetString(Op, TEXT("from")));
            if (!From)
            {
                OutError = FString::Printf(TEXT("replace_expression_references.from unknown expression: %s"), *ABTJson::GetString(Op, TEXT("from")));
                return false;
            }

            UMaterialExpression* To = nullptr;
            int32 ToOutputIndex = 0;
            const FString ToRef = ABTJson::GetString(Op, TEXT("to"));
            if (!ToRef.IsEmpty())
            {
                FString ToId;
                FString ToOutput;
                SplitExpressionPin(ToRef, ToId, ToOutput);
                To = ResolveMaterialExpressionRef(Material, ExpressionsById, ToId);
                if (!To)
                {
                    OutError = FString::Printf(TEXT("replace_expression_references.to unknown expression: %s"), *ToId);
                    return false;
                }

                ToOutputIndex = ResolveExpressionOutputIndex(To, ToOutput);
                if (ToOutputIndex == INDEX_NONE)
                {
                    OutError = FString::Printf(TEXT("replace_expression_references.to unknown output: %s"), *ToRef);
                    return false;
                }
            }

            ReplaceMaterialExpressionReferences(Material, From, To, ToOutputIndex, Messages);
        }
        else if (OpName == TEXT("delete_expression"))
        {
            if (!Material) { OutError = TEXT("delete_expression requires UMaterial"); return false; }
            UMaterialExpression* Expression = ResolveMaterialExpressionRef(Material, ExpressionsById, ABTJson::GetString(Op, TEXT("expression")));
            if (!Expression)
            {
                OutError = FString::Printf(TEXT("delete_expression referenced unknown expression: %s"), *ABTJson::GetString(Op, TEXT("expression")));
                return false;
            }

            UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
            Messages.Add(FString::Printf(TEXT("Deleted expression %s"), *ABTJson::GetString(Op, TEXT("expression"))));
        }
        else if (OpName == TEXT("set_material_settings"))
        {
            if (!ApplyMaterialSettings(Material, Op, Messages, OutError))
            {
                return false;
            }
        }
        else if (OpName == TEXT("repair_expression_collection"))
        {
            if (!RepairMaterialExpressionCollection(Material, Messages, OutError))
            {
                return false;
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
        Material->PostEditChange();
        UMaterialEditingLibrary::RecompileMaterial(Material);
        Material->MarkPackageDirty();
    }

    if (MIC)
    {
        MIC->PostEditChange();
        MIC->MarkPackageDirty();
    }

    bool bSaved = false;
    bool bParentSaved = true;
    if (bSaveOnSuccess)
    {
        Object->MarkPackageDirty();
        bSaved = UEditorAssetLibrary::SaveLoadedAsset(Object, false);
        if (MIC)
        {
            if (UMaterial* ParentMaterial = Cast<UMaterial>(MIC->Parent))
            {
                ParentMaterial->MarkPackageDirty();
                bParentSaved = UEditorAssetLibrary::SaveLoadedAsset(ParentMaterial, false);
            }
        }
    }

    OutJson = ABTJson::Ok();
    TArray<TSharedPtr<FJsonValue>> JsonMessages;
    for (const FString& Message : Messages) JsonMessages.Add(ABTJson::StringValue(Message));
    OutJson->SetArrayField(TEXT("messages"), JsonMessages);
    OutJson->SetBoolField(TEXT("save_requested"), bSaveOnSuccess);
    OutJson->SetBoolField(TEXT("saved"), bSaveOnSuccess ? (bSaved && bParentSaved) : false);
    if (bSaveOnSuccess)
    {
        OutJson->SetBoolField(TEXT("asset_saved"), bSaved);
        if (MIC)
        {
            OutJson->SetBoolField(TEXT("parent_saved"), bParentSaved);
        }
        if (!(bSaved && bParentSaved))
        {
            OutJson->SetStringField(TEXT("save_warning"), TEXT("Patch was applied in the editor session, but SaveLoadedAsset returned false. Check the editor log for file locks or source-control/save errors."));
        }
    }
    return true;
}
