#include "Blueprint/Ops/ABTWidgetBlueprintOps.h"
#include "Utils/ABTJson.h"

#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Blueprint/Utils/ABTBlueprintGraphUtils.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ContentWidget.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/Widget.h"
#include "Blueprint/UserWidget.h"
#include "Engine/Blueprint.h"
#include "Engine/Texture2D.h"
#include "Fonts/SlateFontInfo.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetTextLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "MovieScene.h"
#include "Sections/MovieSceneEventTriggerSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Styling/SlateBrush.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "Runtime/Launch/Resources/Version.h"

#include <initializer_list>

namespace
{
    FWidgetTransform GetWidgetRenderTransform(const UWidget* Widget)
    {
        if (!Widget)
        {
            return FWidgetTransform();
        }
#if ENGINE_MAJOR_VERSION >= 5
        return Widget->GetRenderTransform();
#else
        return Widget->RenderTransform;
#endif
    }

    FSlateFontInfo GetTextBlockFont(const UTextBlock* TextBlock)
    {
        if (!TextBlock)
        {
            return FSlateFontInfo();
        }
#if ENGINE_MAJOR_VERSION >= 5
        return TextBlock->GetFont();
#else
        return TextBlock->Font;
#endif
    }

    void NotifyWidgetVariableAdded(UWidgetBlueprint* WidgetBlueprint, const FName VariableName)
    {
#if ENGINE_MAJOR_VERSION >= 5
        if (WidgetBlueprint)
        {
            WidgetBlueprint->OnVariableAdded(VariableName);
        }
#endif
    }

    void InitializeAsyncActionNode(UK2Node_AsyncAction* Node, UClass* AsyncActionClass, UFunction* FactoryFunction)
    {
        if (!Node || !AsyncActionClass || !FactoryFunction)
        {
            return;
        }
#if ENGINE_MAJOR_VERSION >= 5
        Node->InitializeProxyFromFunction(FactoryFunction);
#else
        if (FNameProperty* FactoryFunctionNameProperty = FindFProperty<FNameProperty>(Node->GetClass(), TEXT("ProxyFactoryFunctionName")))
        {
            FactoryFunctionNameProperty->SetPropertyValue_InContainer(Node, FactoryFunction->GetFName());
        }
        if (FObjectProperty* FactoryClassProperty = FindFProperty<FObjectProperty>(Node->GetClass(), TEXT("ProxyFactoryClass")))
        {
            FactoryClassProperty->SetObjectPropertyValue_InContainer(Node, AsyncActionClass);
        }
        if (FObjectProperty* ProxyClassProperty = FindFProperty<FObjectProperty>(Node->GetClass(), TEXT("ProxyClass")))
        {
            ProxyClassProperty->SetObjectPropertyValue_InContainer(Node, AsyncActionClass);
        }
        if (FNameProperty* ActivateFunctionNameProperty = FindFProperty<FNameProperty>(Node->GetClass(), TEXT("ProxyActivateFunctionName")))
        {
            const FName ActivateName = AsyncActionClass->FindFunctionByName(TEXT("Activate")) ? FName(TEXT("Activate")) : NAME_None;
            ActivateFunctionNameProperty->SetPropertyValue_InContainer(Node, ActivateName);
        }
#endif
    }

    TArray<TSharedPtr<FJsonObject>> ReadPageSpecs(const TSharedPtr<FJsonObject>& Op)
    {
        TArray<TSharedPtr<FJsonObject>> Result;
        const TArray<TSharedPtr<FJsonValue>>* Pages = nullptr;
        if (Op.IsValid() && Op->TryGetArrayField(TEXT("pages"), Pages) && Pages)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Pages)
            {
                if (Value.IsValid() && Value->AsObject().IsValid())
                {
                    Result.Add(Value->AsObject());
                }
            }
        }

        if (Result.Num() == 0)
        {
            for (int32 Index = 0; Index < 3; ++Index)
            {
                TSharedPtr<FJsonObject> Page = ABTJson::Object();
                Page->SetStringField(TEXT("buttonText"), FString::Printf(TEXT("Page %d"), Index + 1));
                Page->SetStringField(TEXT("title"), FString::Printf(TEXT("Sub Page %d"), Index + 1));
                Page->SetStringField(TEXT("body"), TEXT("Created by AgentBlueprintTools."));
                Result.Add(Page);
            }
        }

        return Result;
    }

    FFrameNumber FrameAtSeconds(const UMovieScene* MovieScene, double Seconds)
    {
        return MovieScene ? MovieScene->GetTickResolution().AsFrameNumber(Seconds) : FFrameNumber(0);
    }

    UWidgetAnimation* FindOrCreateWidgetAnimation(UWidgetBlueprint* WidgetBlueprint, const FName AnimationName, const float DurationSeconds)
    {
        if (!WidgetBlueprint)
        {
            return nullptr;
        }

        UWidgetAnimation* Animation = nullptr;
        for (UWidgetAnimation* Candidate : WidgetBlueprint->Animations)
        {
            if (Candidate && Candidate->GetFName() == AnimationName)
            {
                Animation = Candidate;
                break;
            }
        }

        if (!Animation)
        {
            Animation = NewObject<UWidgetAnimation>(WidgetBlueprint, AnimationName, RF_Transactional);
            WidgetBlueprint->Animations.Add(Animation);
            NotifyWidgetVariableAdded(WidgetBlueprint, AnimationName);
        }

        Animation->Modify();
        Animation->SetDisplayLabel(AnimationName.ToString());
        Animation->AnimationBindings.Reset();
        Animation->MovieScene = NewObject<UMovieScene>(Animation, MakeUniqueObjectName(Animation, UMovieScene::StaticClass(), TEXT("MovieScene")), RF_Transactional);
        Animation->MovieScene->Modify();
        Animation->MovieScene->SetDisplayRate(FFrameRate(20, 1));

        const FFrameNumber EndFrame = FrameAtSeconds(Animation->MovieScene, FMath::Max(DurationSeconds, 0.01f));
        Animation->MovieScene->SetPlaybackRange(FFrameNumber(0), EndFrame.Value + 1);
        Animation->MovieScene->GetEditorData().WorkStart = 0.0;
        Animation->MovieScene->GetEditorData().WorkEnd = FMath::Max(DurationSeconds, 0.01f);

        return Animation;
    }

    void BindWidgetToAnimation(UWidgetAnimation* Animation, UWidget* Widget, const FGuid& BindingGuid)
    {
        if (!Animation || !Widget)
        {
            return;
        }

        FWidgetAnimationBinding Binding;
        Binding.WidgetName = Widget->GetFName();
        Binding.SlotWidgetName = NAME_None;
        Binding.AnimationGuid = BindingGuid;
        Binding.bIsRootWidget = false;
        Animation->AnimationBindings.Add(Binding);
    }

    void AddOpacityTrack(UMovieScene* MovieScene, const FGuid& BindingGuid, float InitialOpacity, FFrameNumber EndFrame)
    {
        if (!MovieScene)
        {
            return;
        }

        UMovieSceneFloatTrack* OpacityTrack = MovieScene->AddTrack<UMovieSceneFloatTrack>(BindingGuid);
        if (!OpacityTrack)
        {
            return;
        }

        OpacityTrack->SetPropertyNameAndPath(TEXT("RenderOpacity"), TEXT("RenderOpacity"));
        UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(OpacityTrack->CreateNewSection());
        if (!Section)
        {
            return;
        }

        Section->SetRange(TRange<FFrameNumber>(FFrameNumber(0), EndFrame));
        FMovieSceneFloatChannel& Channel = const_cast<FMovieSceneFloatChannel&>(Section->GetChannel());
        Channel.AddLinearKey(FFrameNumber(0), InitialOpacity);
        Channel.AddLinearKey(EndFrame, 1.0f);
        OpacityTrack->AddSection(*Section);
    }

    void AddScaleTrack(UMovieScene* MovieScene, const FGuid& BindingGuid, const FVector2D& InitialScale, FFrameNumber EndFrame)
    {
        if (!MovieScene)
        {
            return;
        }

        UMovieScene2DTransformTrack* ScaleTrack = MovieScene->AddTrack<UMovieScene2DTransformTrack>(BindingGuid);
        if (!ScaleTrack)
        {
            return;
        }

        ScaleTrack->SetPropertyNameAndPath(TEXT("RenderTransform"), TEXT("RenderTransform"));
        UMovieScene2DTransformSection* Section = Cast<UMovieScene2DTransformSection>(ScaleTrack->CreateNewSection());
        if (!Section)
        {
            return;
        }

        Section->SetRange(TRange<FFrameNumber>(FFrameNumber(0), EndFrame));
        Section->SetMask(FMovieScene2DTransformMask(EMovieScene2DTransformChannel::Scale));
        Section->Scale[0].AddLinearKey(FFrameNumber(0), InitialScale.X);
        Section->Scale[0].AddLinearKey(EndFrame, 1.0f);
        Section->Scale[1].AddLinearKey(FFrameNumber(0), InitialScale.Y);
        Section->Scale[1].AddLinearKey(EndFrame, 1.0f);
        ScaleTrack->AddSection(*Section);
    }

    void AddMidpointEventTrack(UBlueprint* Blueprint, UMovieScene* MovieScene, FFrameNumber MidFrame)
    {
        if (!Blueprint || !MovieScene)
        {
            return;
        }

        UClass* EventClass = Blueprint->GeneratedClass ? Blueprint->GeneratedClass : Blueprint->SkeletonGeneratedClass;
        UFunction* MidpointFunction = EventClass ? EventClass->FindFunctionByName(TEXT("OnPopupAnimationMidpoint")) : nullptr;
        if (!MidpointFunction)
        {
            return;
        }

        UMovieSceneEventTrack* EventTrack = MovieScene->AddMasterTrack<UMovieSceneEventTrack>();
        if (!EventTrack)
        {
            return;
        }

        EventTrack->EventPosition = EFireEventsAtPosition::AtEndOfEvaluation;
        UMovieSceneEventTriggerSection* EventSection = Cast<UMovieSceneEventTriggerSection>(EventTrack->CreateNewSection());
        if (!EventSection)
        {
            return;
        }

        EventSection->SetRange(TRange<FFrameNumber>::All());
        FMovieSceneEvent Event;
        Event.Ptrs.Function = MidpointFunction;
#if WITH_EDITORONLY_DATA
        Event.CompiledFunctionName = MidpointFunction->GetFName();
#endif
        EventSection->EventChannel.GetData().AddKey(MidFrame, Event);
        EventTrack->AddSection(*EventSection);
    }

    void ConfigurePagePopupAnimation(UWidgetBlueprint* WidgetBlueprint, UWidget* PagePanel, int32 PageIndex, float DurationSeconds, float InitialOpacity, const FVector2D& InitialScale)
    {
        if (!WidgetBlueprint || !PagePanel)
        {
            return;
        }

        const FName AnimationName(*FString::Printf(TEXT("PagePopup_%d"), PageIndex));
        UWidgetAnimation* Animation = FindOrCreateWidgetAnimation(WidgetBlueprint, AnimationName, DurationSeconds);
        if (!Animation || !Animation->MovieScene)
        {
            return;
        }

        UMovieScene* MovieScene = Animation->MovieScene;
        const FFrameNumber EndFrame = FrameAtSeconds(MovieScene, FMath::Max(DurationSeconds, 0.01f));
        const FFrameNumber MidFrame = FrameAtSeconds(MovieScene, FMath::Max(DurationSeconds, 0.01f) * 0.5f);
        const FGuid BindingGuid = MovieScene->AddPossessable(PagePanel->GetName(), PagePanel->GetClass());
        BindWidgetToAnimation(Animation, PagePanel, BindingGuid);
        AddOpacityTrack(MovieScene, BindingGuid, InitialOpacity, EndFrame);
        AddScaleTrack(MovieScene, BindingGuid, InitialScale, EndFrame);
        AddMidpointEventTrack(WidgetBlueprint, MovieScene, MidFrame);
    }

    void EnsureWidgetGuids(UWidgetBlueprint* WidgetBlueprint)
    {
#if ENGINE_MAJOR_VERSION >= 5
        if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
        {
            return;
        }

        TSet<FName> LiveVariableNames;
        WidgetBlueprint->ForEachSourceWidget([WidgetBlueprint, &LiveVariableNames](UWidget* Widget)
        {
            if (!Widget)
            {
                return;
            }

            const FName WidgetName = Widget->GetFName();
            LiveVariableNames.Add(WidgetName);
            if (!WidgetBlueprint->WidgetVariableNameToGuidMap.Contains(WidgetName))
            {
                WidgetBlueprint->OnVariableAdded(WidgetName);
            }
        });

        for (UWidgetAnimation* Animation : WidgetBlueprint->Animations)
        {
            if (!Animation)
            {
                continue;
            }

            const FName AnimationName = Animation->GetFName();
            LiveVariableNames.Add(AnimationName);
            if (!WidgetBlueprint->WidgetVariableNameToGuidMap.Contains(AnimationName))
            {
                WidgetBlueprint->OnVariableAdded(AnimationName);
            }
        }

        TArray<FName> StaleVariableNames;
        for (const TPair<FName, FGuid>& Entry : WidgetBlueprint->WidgetVariableNameToGuidMap)
        {
            if (!LiveVariableNames.Contains(Entry.Key))
            {
                StaleVariableNames.Add(Entry.Key);
            }
        }

        for (const FName& StaleName : StaleVariableNames)
        {
            WidgetBlueprint->OnVariableRemoved(StaleName);
        }
#else
        (void)WidgetBlueprint;
#endif
    }

    FString SanitizeWidgetName(const FString& RawName, const FString& Fallback)
    {
        FString Result;
        const FString Source = RawName.IsEmpty() ? Fallback : RawName;
        for (const TCHAR Ch : Source)
        {
            Result.AppendChar((FChar::IsAlnum(Ch) || Ch == TEXT('_')) ? Ch : TEXT('_'));
        }
        Result.TrimStartAndEndInline();
        if (Result.IsEmpty())
        {
            Result = Fallback;
        }
        if (Result.Len() > 0 && FChar::IsDigit(Result[0]))
        {
            Result = TEXT("W_") + Result;
        }
        return Result;
    }

    FString GetFirstStringField(const TSharedPtr<FJsonObject>& Object, std::initializer_list<const TCHAR*> Fields)
    {
        for (const TCHAR* Field : Fields)
        {
            const FString Value = ABTJson::GetString(Object, Field);
            if (!Value.IsEmpty())
            {
                return Value;
            }
        }
        return FString();
    }

    FString ToObjectPath(const FString& AssetPath)
    {
        if (AssetPath.Contains(TEXT(".")))
        {
            return AssetPath;
        }

        int32 SlashIndex = INDEX_NONE;
        if (!AssetPath.FindLastChar(TEXT('/'), SlashIndex) || SlashIndex == INDEX_NONE)
        {
            return AssetPath;
        }

        const FString AssetName = AssetPath.Mid(SlashIndex + 1);
        return AssetPath + TEXT(".") + AssetName;
    }

    bool TryReadNumber(const TSharedPtr<FJsonObject>& Object, const FString& Field, double& OutValue)
    {
        return Object.IsValid() && Object->TryGetNumberField(Field, OutValue);
    }

    bool TryReadVec2(const TSharedPtr<FJsonObject>& Object, const FString& Field, FVector2D& OutValue)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || !Values || Values->Num() < 2)
        {
            return false;
        }

        OutValue = FVector2D((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber());
        return true;
    }

    bool TryReadPosition(const TSharedPtr<FJsonObject>& Object, FVector2D& OutValue)
    {
        if (TryReadVec2(Object, TEXT("position"), OutValue))
        {
            return true;
        }

        double X = 0.0;
        double Y = 0.0;
        const bool bHasX = TryReadNumber(Object, TEXT("x"), X);
        const bool bHasY = TryReadNumber(Object, TEXT("y"), Y);
        if (bHasX || bHasY)
        {
            OutValue = FVector2D(X, Y);
            return true;
        }

        return false;
    }

    bool TryReadSize(const TSharedPtr<FJsonObject>& Object, FVector2D& OutValue)
    {
        if (TryReadVec2(Object, TEXT("size"), OutValue))
        {
            return true;
        }

        double Width = 0.0;
        double Height = 0.0;
        const bool bHasWidth = TryReadNumber(Object, TEXT("width"), Width);
        const bool bHasHeight = TryReadNumber(Object, TEXT("height"), Height);
        if (bHasWidth || bHasHeight)
        {
            OutValue = FVector2D(Width, Height);
            return true;
        }

        return false;
    }

    bool TryReadColor(const TSharedPtr<FJsonObject>& Object, const FString& Field, FLinearColor& OutColor)
    {
        const TSharedPtr<FJsonValue> Value = Object.IsValid() ? Object->TryGetField(Field) : nullptr;
        if (!Value.IsValid())
        {
            return false;
        }

        if (Value->Type == EJson::String)
        {
            FString Hex = Value->AsString();
            Hex.TrimStartAndEndInline();
            if (Hex.StartsWith(TEXT("#")))
            {
                Hex.RightChopInline(1);
            }
            if (Hex.Len() == 6 || Hex.Len() == 8)
            {
                OutColor = FLinearColor::FromSRGBColor(FColor::FromHex(Hex));
                return true;
            }
            return false;
        }

        if (Value->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
            if (Values.Num() < 3)
            {
                return false;
            }

            double R = Values[0]->AsNumber();
            double G = Values[1]->AsNumber();
            double B = Values[2]->AsNumber();
            double A = Values.Num() > 3 ? Values[3]->AsNumber() : 1.0;
            if (R > 1.0 || G > 1.0 || B > 1.0 || A > 1.0)
            {
                R /= 255.0;
                G /= 255.0;
                B /= 255.0;
                if (A > 1.0)
                {
                    A /= 255.0;
                }
            }
            OutColor = FLinearColor(R, G, B, A);
            return true;
        }

        return false;
    }

    FMargin ReadMargin(const TSharedPtr<FJsonObject>& Object, const FString& Field, const FMargin& DefaultValue = FMargin())
    {
        const TSharedPtr<FJsonValue> Value = Object.IsValid() ? Object->TryGetField(Field) : nullptr;
        if (!Value.IsValid())
        {
            return DefaultValue;
        }

        if (Value->Type == EJson::Number)
        {
            const float Uniform = static_cast<float>(Value->AsNumber());
            return FMargin(Uniform);
        }

        if (Value->Type != EJson::Array)
        {
            return DefaultValue;
        }

        const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
        if (Values.Num() >= 4)
        {
            return FMargin(
                static_cast<float>(Values[0]->AsNumber()),
                static_cast<float>(Values[1]->AsNumber()),
                static_cast<float>(Values[2]->AsNumber()),
                static_cast<float>(Values[3]->AsNumber()));
        }
        if (Values.Num() == 2)
        {
            return FMargin(
                static_cast<float>(Values[0]->AsNumber()),
                static_cast<float>(Values[1]->AsNumber()));
        }
        if (Values.Num() == 1)
        {
            return FMargin(static_cast<float>(Values[0]->AsNumber()));
        }

        return DefaultValue;
    }

    EHorizontalAlignment ReadHorizontalAlignment(const TSharedPtr<FJsonObject>& Object, EHorizontalAlignment DefaultValue = HAlign_Fill)
    {
        const FString Value = GetFirstStringField(Object, { TEXT("horizontalAlignment"), TEXT("hAlign") }).ToLower();
        if (Value == TEXT("left")) return HAlign_Left;
        if (Value == TEXT("center") || Value == TEXT("centre")) return HAlign_Center;
        if (Value == TEXT("right")) return HAlign_Right;
        if (Value == TEXT("fill")) return HAlign_Fill;
        return DefaultValue;
    }

    EVerticalAlignment ReadVerticalAlignment(const TSharedPtr<FJsonObject>& Object, EVerticalAlignment DefaultValue = VAlign_Fill)
    {
        const FString Value = GetFirstStringField(Object, { TEXT("verticalAlignment"), TEXT("vAlign") }).ToLower();
        if (Value == TEXT("top")) return VAlign_Top;
        if (Value == TEXT("center") || Value == TEXT("centre")) return VAlign_Center;
        if (Value == TEXT("bottom")) return VAlign_Bottom;
        if (Value == TEXT("fill")) return VAlign_Fill;
        return DefaultValue;
    }

    ETextJustify::Type ReadJustification(const TSharedPtr<FJsonObject>& Object)
    {
        const FString Value = GetFirstStringField(Object, { TEXT("justification"), TEXT("textAlign") }).ToLower();
        if (Value == TEXT("center") || Value == TEXT("centre")) return ETextJustify::Center;
        if (Value == TEXT("right")) return ETextJustify::Right;
        return ETextJustify::Left;
    }

    UTexture2D* LoadTextureFromPath(const FString& RawPath)
    {
        if (RawPath.IsEmpty())
        {
            return nullptr;
        }

        if (UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *RawPath))
        {
            return Texture;
        }
        return LoadObject<UTexture2D>(nullptr, *ToObjectPath(RawPath));
    }

    UClass* LoadWidgetClassFromPath(const FString& RawPath)
    {
        if (RawPath.IsEmpty())
        {
            return nullptr;
        }

        if (RawPath.EndsWith(TEXT("_C")) || RawPath.StartsWith(TEXT("/Script/")))
        {
            if (UClass* DirectClass = LoadObject<UClass>(nullptr, *RawPath))
            {
                return DirectClass;
            }
        }

        UObject* Object = LoadObject<UObject>(nullptr, *RawPath);
        if (!Object)
        {
            Object = LoadObject<UObject>(nullptr, *ToObjectPath(RawPath));
        }

        if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
        {
            return Blueprint->GeneratedClass ? Blueprint->GeneratedClass.Get() : Blueprint->SkeletonGeneratedClass.Get();
        }
        if (UClass* Class = Cast<UClass>(Object))
        {
            return Class;
        }

        const FString ObjectPath = ToObjectPath(RawPath);
        return LoadObject<UClass>(nullptr, *(ObjectPath + TEXT("_C")));
    }

    UObject* LoadObjectValueForProperty(FObjectProperty* ObjectProperty, const FString& RawPath)
    {
        if (!ObjectProperty || RawPath.IsEmpty())
        {
            return nullptr;
        }

        if (UObject* Object = LoadObject<UObject>(nullptr, *RawPath))
        {
            return Object;
        }

        if (UObject* Object = LoadObject<UObject>(nullptr, *ToObjectPath(RawPath)))
        {
            return Object;
        }

        return nullptr;
    }

    bool SetJsonValueOnObject(UObject* Object, const FString& PropertyName, const TSharedPtr<FJsonValue>& Value, FString& OutError)
    {
        if (!Object)
        {
            OutError = TEXT("object is null");
            return false;
        }
        if (PropertyName.IsEmpty())
        {
            OutError = TEXT("property name is empty");
            return false;
        }
        if (!Value.IsValid())
        {
            OutError = FString::Printf(TEXT("value missing for property %s"), *PropertyName);
            return false;
        }

        FProperty* Property = Object->GetClass()->FindPropertyByName(*PropertyName);
        if (!Property)
        {
            OutError = FString::Printf(TEXT("Property not found on %s: %s"), *Object->GetClass()->GetName(), *PropertyName);
            return false;
        }

        void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
        if (FBoolProperty* Bool = CastField<FBoolProperty>(Property))
        {
            Bool->SetPropertyValue(ValuePtr, Value->AsBool());
            return true;
        }
        if (FNumericProperty* Number = CastField<FNumericProperty>(Property))
        {
            if (Number->IsInteger())
            {
                Number->SetIntPropertyValue(ValuePtr, static_cast<int64>(Value->AsNumber()));
            }
            else
            {
                Number->SetFloatingPointPropertyValue(ValuePtr, Value->AsNumber());
            }
            return true;
        }
        if (FStrProperty* String = CastField<FStrProperty>(Property))
        {
            String->SetPropertyValue(ValuePtr, Value->AsString());
            return true;
        }
        if (FNameProperty* Name = CastField<FNameProperty>(Property))
        {
            Name->SetPropertyValue(ValuePtr, *Value->AsString());
            return true;
        }
        if (FTextProperty* Text = CastField<FTextProperty>(Property))
        {
            Text->SetPropertyValue(ValuePtr, FText::FromString(Value->AsString()));
            return true;
        }
        if (FEnumProperty* Enum = CastField<FEnumProperty>(Property))
        {
            const int64 EnumValue = Enum->GetEnum()->GetValueByNameString(Value->AsString());
            if (EnumValue == INDEX_NONE)
            {
                OutError = FString::Printf(TEXT("Invalid enum value '%s' for %s"), *Value->AsString(), *PropertyName);
                return false;
            }
            Enum->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, EnumValue);
            return true;
        }
        if (FByteProperty* Byte = CastField<FByteProperty>(Property))
        {
            if (Byte->Enum)
            {
                const int64 EnumValue = Byte->Enum->GetValueByNameString(Value->AsString());
                if (EnumValue == INDEX_NONE)
                {
                    OutError = FString::Printf(TEXT("Invalid enum value '%s' for %s"), *Value->AsString(), *PropertyName);
                    return false;
                }
                Byte->SetPropertyValue(ValuePtr, static_cast<uint8>(EnumValue));
            }
            else
            {
                Byte->SetPropertyValue(ValuePtr, static_cast<uint8>(Value->AsNumber()));
            }
            return true;
        }
        if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
        {
            UObject* Referenced = nullptr;
            const FString ObjectPath = Value->AsString();
            if (!ObjectPath.IsEmpty())
            {
                Referenced = LoadObjectValueForProperty(ObjectProperty, ObjectPath);
                if (!Referenced)
                {
                    OutError = FString::Printf(TEXT("Could not load object value: %s"), *ObjectPath);
                    return false;
                }
                if (!Referenced->IsA(ObjectProperty->PropertyClass))
                {
                    OutError = FString::Printf(TEXT("Object value is not a %s: %s"), *ObjectProperty->PropertyClass->GetName(), *ObjectPath);
                    return false;
                }
            }
            ObjectProperty->SetObjectPropertyValue(ValuePtr, Referenced);
            return true;
        }

        OutError = FString::Printf(TEXT("Unsupported property type for %s: %s"), *PropertyName, *Property->GetClass()->GetName());
        return false;
    }

    bool ApplyUserWidgetInstanceProperties(UUserWidget* UserWidget, const TSharedPtr<FJsonObject>& Spec, FString& OutError)
    {
        if (!UserWidget || !Spec.IsValid())
        {
            return true;
        }

        const TSharedPtr<FJsonObject>* Properties = nullptr;
        if (!Spec->TryGetObjectField(TEXT("properties"), Properties))
        {
            Spec->TryGetObjectField(TEXT("instanceProperties"), Properties);
        }
        if (!Properties || !Properties->IsValid())
        {
            return true;
        }

        UserWidget->Modify();
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Properties)->Values)
        {
            if (!SetJsonValueOnObject(UserWidget, Pair.Key, Pair.Value, OutError))
            {
                return false;
            }
        }
        return true;
    }

    void ApplyCommonWidgetProperties(UWidget* Widget, const TSharedPtr<FJsonObject>& Spec)
    {
        if (!Widget || !Spec.IsValid())
        {
            return;
        }

        Widget->bIsVariable = ABTJson::GetBool(Spec, TEXT("isVariable"), ABTJson::GetBool(Spec, TEXT("variable"), false));
        if (Spec->HasField(TEXT("renderOpacity")))
        {
            Widget->SetRenderOpacity(static_cast<float>(ABTJson::GetNumber(Spec, TEXT("renderOpacity"), 1.0)));
        }

        FVector2D RenderScale;
        if (TryReadVec2(Spec, TEXT("renderScale"), RenderScale))
        {
            FWidgetTransform Transform = GetWidgetRenderTransform(Widget);
            Transform.Scale = RenderScale;
            Widget->SetRenderTransform(Transform);
        }

        const FString Visibility = ABTJson::GetString(Spec, TEXT("visibility"));
        if (!Visibility.IsEmpty())
        {
            if (const UEnum* Enum = StaticEnum<ESlateVisibility>())
            {
                const int64 Value = Enum->GetValueByNameString(Visibility);
                if (Value != INDEX_NONE)
                {
                    Widget->SetVisibility(static_cast<ESlateVisibility>(Value));
                }
            }
        }
    }

    bool ApplyWidgetSpecificProperties(UWidget* Widget, const TSharedPtr<FJsonObject>& Spec, FString& OutError)
    {
        if (!Widget || !Spec.IsValid())
        {
            OutError = TEXT("Invalid widget spec");
            return false;
        }

        if (UTextBlock* TextBlock = Cast<UTextBlock>(Widget))
        {
            const TSharedPtr<FJsonValue> TextValue = Spec->TryGetField(TEXT("text"));
            if (TextValue.IsValid())
            {
                TextBlock->SetText(FText::FromString(TextValue->AsString()));
            }
            TextBlock->SetAutoWrapText(ABTJson::GetBool(Spec, TEXT("autoWrap"), false));
            TextBlock->SetJustification(ReadJustification(Spec));

            const double FontSize = ABTJson::GetNumber(Spec, TEXT("fontSize"), 0.0);
            if (FontSize > 0.0)
            {
                FSlateFontInfo Font = GetTextBlockFont(TextBlock);
                Font.Size = static_cast<int32>(FontSize);
                TextBlock->SetFont(Font);
            }

            FLinearColor TextColor;
            if (TryReadColor(Spec, TEXT("color"), TextColor))
            {
                TextBlock->SetColorAndOpacity(FSlateColor(TextColor));
            }
        }
        else if (UBorder* Border = Cast<UBorder>(Widget))
        {
            Border->SetPadding(ReadMargin(Spec, TEXT("padding"), FMargin()));

            FLinearColor BrushColor;
            if (TryReadColor(Spec, TEXT("backgroundColor"), BrushColor) || TryReadColor(Spec, TEXT("color"), BrushColor))
            {
                Border->SetBrushColor(BrushColor);
            }

            const FString TexturePath = GetFirstStringField(Spec, { TEXT("brushPath"), TEXT("texturePath"), TEXT("imagePath") });
            if (!TexturePath.IsEmpty())
            {
                if (UTexture2D* Texture = LoadTextureFromPath(TexturePath))
                {
                    Border->SetBrushFromTexture(Texture);
                }
                else
                {
                    OutError = FString::Printf(TEXT("Texture not found for border %s: %s"), *Widget->GetName(), *TexturePath);
                    return false;
                }
            }
        }
        else if (UImage* Image = Cast<UImage>(Widget))
        {
            const FString TexturePath = GetFirstStringField(Spec, { TEXT("brushPath"), TEXT("texturePath"), TEXT("imagePath") });
            if (!TexturePath.IsEmpty())
            {
                if (UTexture2D* Texture = LoadTextureFromPath(TexturePath))
                {
                    Image->SetBrushFromTexture(Texture, false);
                }
                else
                {
                    OutError = FString::Printf(TEXT("Texture not found for image %s: %s"), *Widget->GetName(), *TexturePath);
                    return false;
                }
            }

            FLinearColor Tint;
            if (TryReadColor(Spec, TEXT("color"), Tint))
            {
                Image->SetColorAndOpacity(Tint);
            }
        }
        else if (USizeBox* SizeBox = Cast<USizeBox>(Widget))
        {
            FVector2D Size;
            if (TryReadSize(Spec, Size))
            {
                SizeBox->SetWidthOverride(Size.X);
                SizeBox->SetHeightOverride(Size.Y);
            }
        }
        else if (USpacer* Spacer = Cast<USpacer>(Widget))
        {
            FVector2D Size;
            if (TryReadSize(Spec, Size))
            {
                Spacer->SetSize(Size);
            }
        }

        if (UUserWidget* UserWidget = Cast<UUserWidget>(Widget))
        {
            return ApplyUserWidgetInstanceProperties(UserWidget, Spec, OutError);
        }

        return true;
    }

    void ApplySlotProperties(UPanelSlot* Slot, const TSharedPtr<FJsonObject>& Spec)
    {
        if (!Slot || !Spec.IsValid())
        {
            return;
        }

        if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
        {
            FVector2D Position;
            if (TryReadPosition(Spec, Position))
            {
                CanvasSlot->SetPosition(Position);
            }

            FVector2D Size;
            if (TryReadSize(Spec, Size))
            {
                CanvasSlot->SetSize(Size);
            }

            FVector2D Alignment;
            if (TryReadVec2(Spec, TEXT("alignment"), Alignment))
            {
                CanvasSlot->SetAlignment(Alignment);
            }
            CanvasSlot->SetAutoSize(ABTJson::GetBool(Spec, TEXT("autoSize"), false));
            CanvasSlot->SetZOrder(ABTJson::GetInt(Spec, TEXT("zOrder"), ABTJson::GetInt(Spec, TEXT("z"), 0)));
        }
        else if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(Slot))
        {
            OverlaySlot->SetPadding(ReadMargin(Spec, TEXT("padding"), FMargin()));
            OverlaySlot->SetHorizontalAlignment(ReadHorizontalAlignment(Spec));
            OverlaySlot->SetVerticalAlignment(ReadVerticalAlignment(Spec));
        }
        else if (UVerticalBoxSlot* VerticalSlot = Cast<UVerticalBoxSlot>(Slot))
        {
            VerticalSlot->SetPadding(ReadMargin(Spec, TEXT("padding"), FMargin()));
            VerticalSlot->SetHorizontalAlignment(ReadHorizontalAlignment(Spec));
            VerticalSlot->SetVerticalAlignment(ReadVerticalAlignment(Spec, VAlign_Center));
            if (ABTJson::GetString(Spec, TEXT("sizeRule")).Equals(TEXT("Fill"), ESearchCase::IgnoreCase))
            {
                VerticalSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
            }
        }
        else if (UHorizontalBoxSlot* HorizontalSlot = Cast<UHorizontalBoxSlot>(Slot))
        {
            HorizontalSlot->SetPadding(ReadMargin(Spec, TEXT("padding"), FMargin()));
            HorizontalSlot->SetHorizontalAlignment(ReadHorizontalAlignment(Spec, HAlign_Center));
            HorizontalSlot->SetVerticalAlignment(ReadVerticalAlignment(Spec));
            if (ABTJson::GetString(Spec, TEXT("sizeRule")).Equals(TEXT("Fill"), ESearchCase::IgnoreCase))
            {
                HorizontalSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
            }
        }
    }

    UClass* ResolveFigmaWidgetClass(const TSharedPtr<FJsonObject>& Spec, FString& OutError)
    {
        const FString Type = ABTJson::GetString(Spec, TEXT("type"), TEXT("CanvasPanel"));
        if (Type.Equals(TEXT("CanvasPanel"), ESearchCase::IgnoreCase) ||
            Type.Equals(TEXT("Canvas"), ESearchCase::IgnoreCase) ||
            Type.Equals(TEXT("Frame"), ESearchCase::IgnoreCase))
        {
            return UCanvasPanel::StaticClass();
        }
        if (Type.Equals(TEXT("Overlay"), ESearchCase::IgnoreCase))
        {
            return UOverlay::StaticClass();
        }
        if (Type.Equals(TEXT("Border"), ESearchCase::IgnoreCase) ||
            Type.Equals(TEXT("Rectangle"), ESearchCase::IgnoreCase))
        {
            return UBorder::StaticClass();
        }
        if (Type.Equals(TEXT("TextBlock"), ESearchCase::IgnoreCase) ||
            Type.Equals(TEXT("Text"), ESearchCase::IgnoreCase))
        {
            return UTextBlock::StaticClass();
        }
        if (Type.Equals(TEXT("Image"), ESearchCase::IgnoreCase) ||
            Type.Equals(TEXT("Icon"), ESearchCase::IgnoreCase))
        {
            return UImage::StaticClass();
        }
        if (Type.Equals(TEXT("VerticalBox"), ESearchCase::IgnoreCase))
        {
            return UVerticalBox::StaticClass();
        }
        if (Type.Equals(TEXT("HorizontalBox"), ESearchCase::IgnoreCase))
        {
            return UHorizontalBox::StaticClass();
        }
        if (Type.Equals(TEXT("Button"), ESearchCase::IgnoreCase))
        {
            return UButton::StaticClass();
        }
        if (Type.Equals(TEXT("SizeBox"), ESearchCase::IgnoreCase))
        {
            return USizeBox::StaticClass();
        }
        if (Type.Equals(TEXT("Spacer"), ESearchCase::IgnoreCase))
        {
            return USpacer::StaticClass();
        }

        const FString WidgetClassPath = GetFirstStringField(Spec, { TEXT("widgetClass"), TEXT("widgetAsset"), TEXT("class") });
        if (!WidgetClassPath.IsEmpty())
        {
            if (UClass* WidgetClass = LoadWidgetClassFromPath(WidgetClassPath))
            {
                if (!WidgetClass->IsChildOf(UWidget::StaticClass()))
                {
                    OutError = FString::Printf(TEXT("Resolved class is not a UWidget: %s"), *WidgetClassPath);
                    return nullptr;
                }
                return WidgetClass;
            }

            OutError = FString::Printf(TEXT("Widget class or asset not found: %s"), *WidgetClassPath);
            return nullptr;
        }

        OutError = FString::Printf(TEXT("Unsupported figma widget type: %s"), *Type);
        return nullptr;
    }

    UWidget* ConstructFigmaWidget(UWidgetTree* Tree, const TSharedPtr<FJsonObject>& Spec, int32 FallbackIndex, FString& OutError)
    {
        if (!Tree || !Spec.IsValid())
        {
            OutError = TEXT("Invalid figma widget spec");
            return nullptr;
        }

        UClass* WidgetClass = ResolveFigmaWidgetClass(Spec, OutError);
        if (!WidgetClass)
        {
            return nullptr;
        }

        const FString FallbackName = FString::Printf(TEXT("FigmaWidget_%d"), FallbackIndex);
        const FString SanitizedName = SanitizeWidgetName(ABTJson::GetString(Spec, TEXT("name")), FallbackName);
        const FName WidgetName = MakeUniqueObjectName(Tree, WidgetClass, *SanitizedName);

        if (WidgetClass->IsChildOf(UUserWidget::StaticClass()))
        {
            return Tree->ConstructWidget<UUserWidget>(WidgetClass, WidgetName);
        }
        return Tree->ConstructWidget<UWidget>(WidgetClass, WidgetName);
    }

    bool AddFigmaChild(UWidget* Parent, UWidget* Child, const TSharedPtr<FJsonObject>& ChildSpec, FString& OutError)
    {
        if (!Parent || !Child)
        {
            return true;
        }

        UPanelSlot* Slot = nullptr;
        if (UCanvasPanel* CanvasPanel = Cast<UCanvasPanel>(Parent))
        {
            Slot = CanvasPanel->AddChildToCanvas(Child);
        }
        else if (UOverlay* Overlay = Cast<UOverlay>(Parent))
        {
            Slot = Overlay->AddChildToOverlay(Child);
        }
        else if (UVerticalBox* VerticalBox = Cast<UVerticalBox>(Parent))
        {
            Slot = VerticalBox->AddChildToVerticalBox(Child);
        }
        else if (UHorizontalBox* HorizontalBox = Cast<UHorizontalBox>(Parent))
        {
            Slot = HorizontalBox->AddChildToHorizontalBox(Child);
        }
        else if (UContentWidget* ContentWidget = Cast<UContentWidget>(Parent))
        {
            if (ContentWidget->GetContent())
            {
                OutError = FString::Printf(TEXT("%s can only contain one child"), *Parent->GetName());
                return false;
            }
            Slot = ContentWidget->SetContent(Child);
        }
        else if (UPanelWidget* Panel = Cast<UPanelWidget>(Parent))
        {
            Slot = Panel->AddChild(Child);
        }
        else
        {
            OutError = FString::Printf(TEXT("%s cannot contain child widget %s"), *Parent->GetName(), *Child->GetName());
            return false;
        }

        if (!Slot)
        {
            OutError = FString::Printf(TEXT("Failed to add %s to %s"), *Child->GetName(), *Parent->GetName());
            return false;
        }

        ApplySlotProperties(Slot, ChildSpec);
        return true;
    }

    bool BuildFigmaWidgetTree(
        UWidgetTree* Tree,
        const TSharedPtr<FJsonObject>& Spec,
        UWidget* Parent,
        int32& WidgetIndex,
        UWidget*& OutWidget,
        FString& OutError)
    {
        OutWidget = ConstructFigmaWidget(Tree, Spec, WidgetIndex++, OutError);
        if (!OutWidget)
        {
            return false;
        }

        ApplyCommonWidgetProperties(OutWidget, Spec);
        if (!ApplyWidgetSpecificProperties(OutWidget, Spec, OutError))
        {
            return false;
        }

        if (!AddFigmaChild(Parent, OutWidget, Spec, OutError))
        {
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
        if (Spec->TryGetArrayField(TEXT("children"), Children) && Children)
        {
            for (const TSharedPtr<FJsonValue>& ChildValue : *Children)
            {
                const TSharedPtr<FJsonObject> ChildSpec = ChildValue->AsObject();
                if (!ChildSpec.IsValid())
                {
                    OutError = FString::Printf(TEXT("children entry under %s is not an object"), *OutWidget->GetName());
                    return false;
                }

                UWidget* ChildWidget = nullptr;
                if (!BuildFigmaWidgetTree(Tree, ChildSpec, OutWidget, WidgetIndex, ChildWidget, OutError))
                {
                    return false;
                }
            }
        }

        return true;
    }

    UScriptStruct* LoadScriptStructFromPath(const FString& RawPath)
    {
        if (RawPath.IsEmpty())
        {
            return nullptr;
        }

        if (UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *RawPath))
        {
            return Struct;
        }

        if (!RawPath.StartsWith(TEXT("/Script/")))
        {
            return LoadObject<UScriptStruct>(nullptr, *FString::Printf(TEXT("/Script/%s"), *RawPath));
        }

        return nullptr;
    }

    UTexture2D* RequireTexture(const TSharedPtr<FJsonObject>& Op, const FString& Field, FString& OutError)
    {
        const FString TexturePath = ABTJson::GetString(Op, Field);
        UTexture2D* Texture = LoadTextureFromPath(TexturePath);
        if (!Texture)
        {
            OutError = FString::Printf(TEXT("Texture not found for %s: %s"), *Field, *TexturePath);
        }
        return Texture;
    }

    UFunction* RequireFunction(UClass* OwnerClass, const FName FunctionName, FString& OutError)
    {
        UFunction* Function = OwnerClass ? OwnerClass->FindFunctionByName(FunctionName) : nullptr;
        if (!Function)
        {
            OutError = FString::Printf(TEXT("Function not found: %s.%s"), OwnerClass ? *OwnerClass->GetName() : TEXT("<null>"), *FunctionName.ToString());
        }
        return Function;
    }

    template <typename TNode>
    TNode* AddTypedNode(UEdGraph* Graph, const int32 X, const int32 Y)
    {
        TNode* Node = NewObject<TNode>(Graph);
        Graph->AddNode(Node, true, false);
        Node->CreateNewGuid();
        Node->NodePosX = X;
        Node->NodePosY = Y;
        return Node;
    }

    UK2Node_CallFunction* AddCallFunctionNode(UEdGraph* Graph, UFunction* Function, const int32 X, const int32 Y)
    {
        if (!Graph || !Function)
        {
            return nullptr;
        }

        UK2Node_CallFunction* Node = AddTypedNode<UK2Node_CallFunction>(Graph, X, Y);
        Node->SetFromFunction(Function);
        Node->AllocateDefaultPins();
        return Node;
    }

    UK2Node_VariableGet* AddVariableGetNode(UEdGraph* Graph, const FName VariableName, const int32 X, const int32 Y)
    {
        UK2Node_VariableGet* Node = AddTypedNode<UK2Node_VariableGet>(Graph, X, Y);
        Node->VariableReference.SetSelfMember(VariableName);
        Node->AllocateDefaultPins();
        return Node;
    }

    UK2Node_VariableSet* AddVariableSetNode(UEdGraph* Graph, const FName VariableName, const int32 X, const int32 Y)
    {
        UK2Node_VariableSet* Node = AddTypedNode<UK2Node_VariableSet>(Graph, X, Y);
        Node->VariableReference.SetSelfMember(VariableName);
        Node->AllocateDefaultPins();
        return Node;
    }

    UK2Node_Event* AddWidgetEventNode(UEdGraph* Graph, const FName EventName, const int32 X, const int32 Y)
    {
        UK2Node_Event* Node = AddTypedNode<UK2Node_Event>(Graph, X, Y);
        Node->EventReference.SetExternalMember(EventName, UUserWidget::StaticClass());
        Node->bOverrideFunction = true;
        Node->AllocateDefaultPins();
        return Node;
    }

    bool ConnectNodePins(UEdGraphNode* FromNode, const FString& FromPinName, UEdGraphNode* ToNode, const FString& ToPinName, FString& OutError)
    {
        UEdGraphPin* FromPin = ABT::Blueprint::FindPinByName(FromNode, FromPinName);
        UEdGraphPin* ToPin = ABT::Blueprint::FindPinByName(ToNode, ToPinName);
        if (!FromPin || !ToPin)
        {
            OutError = FString::Printf(TEXT("Pin not found while connecting %s.%s -> %s.%s"),
                FromNode ? *FromNode->GetName() : TEXT("<null>"),
                *FromPinName,
                ToNode ? *ToNode->GetName() : TEXT("<null>"),
                *ToPinName);
            return false;
        }

        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        if (!Schema->TryCreateConnection(FromPin, ToPin))
        {
            OutError = FString::Printf(TEXT("Could not connect %s.%s -> %s.%s"),
                *FromNode->GetName(), *FromPinName, *ToNode->GetName(), *ToPinName);
            return false;
        }
        return true;
    }

    bool SetObjectPinDefault(UEdGraphNode* Node, const FString& PinName, UObject* Object, FString& OutError)
    {
        UEdGraphPin* Pin = ABT::Blueprint::FindPinByName(Node, PinName);
        if (!Pin)
        {
            OutError = FString::Printf(TEXT("Pin not found: %s.%s"), Node ? *Node->GetName() : TEXT("<null>"), *PinName);
            return false;
        }

        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        Schema->TrySetDefaultObject(*Pin, Object, false);
        return true;
    }

    bool EnsureAsyncActionVariable(UBlueprint* Blueprint, const FName VariableName, UClass* AsyncActionClass, TArray<FString>& OutMessages)
    {
        if (!Blueprint || !AsyncActionClass)
        {
            return false;
        }

        for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
        {
            if (Variable.VarName == VariableName)
            {
                return true;
            }
        }

        FEdGraphPinType PinType;
        PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
        PinType.PinSubCategoryObject = AsyncActionClass;
        FBlueprintEditorUtils::AddMemberVariable(Blueprint, VariableName, PinType);
        OutMessages.Add(FString::Printf(TEXT("Added async action variable %s"), *VariableName.ToString()));
        return true;
    }

    UK2Node_AsyncAction* AddListenForGameplayMessagesNode(UEdGraph* Graph, UScriptStruct* PayloadStruct, const FString& Channel, const int32 X, const int32 Y, FString& OutError)
    {
        if (!FModuleManager::Get().IsModuleLoaded(TEXT("GameplayMessageNodes")))
        {
            FModuleManager::Get().LoadModule(TEXT("GameplayMessageNodes"));
        }

        UClass* AsyncNodeClass = LoadClass<UEdGraphNode>(nullptr, TEXT("/Script/GameplayMessageNodes.K2Node_AsyncAction_ListenForGameplayMessages"));
        UClass* AsyncActionClass = LoadObject<UClass>(nullptr, TEXT("/Script/GameplayMessageRuntime.AsyncAction_ListenForGameplayMessage"));
        if (!AsyncNodeClass || !AsyncNodeClass->IsChildOf(UK2Node_AsyncAction::StaticClass()))
        {
            OutError = TEXT("Could not load K2Node_AsyncAction_ListenForGameplayMessages");
            return nullptr;
        }
        if (!AsyncActionClass)
        {
            OutError = TEXT("Could not load AsyncAction_ListenForGameplayMessage");
            return nullptr;
        }

        UFunction* ListenFunction = AsyncActionClass->FindFunctionByName(TEXT("ListenForGameplayMessages"));
        if (!ListenFunction)
        {
            OutError = TEXT("Could not find ListenForGameplayMessages function");
            return nullptr;
        }

        UK2Node_AsyncAction* Node = NewObject<UK2Node_AsyncAction>(Graph, AsyncNodeClass);
        InitializeAsyncActionNode(Node, AsyncActionClass, ListenFunction);
        Graph->AddNode(Node, true, false);
        Node->CreateNewGuid();
        Node->NodePosX = X;
        Node->NodePosY = Y;
        Node->AllocateDefaultPins();

        if (UEdGraphPin* ChannelPin = ABT::Blueprint::FindPinByName(Node, TEXT("Channel")))
        {
            ChannelPin->DefaultValue = FString::Printf(TEXT("(TagName=\"%s\")"), *Channel);
        }

        if (UEdGraphPin* PayloadTypePin = ABT::Blueprint::FindPinByName(Node, TEXT("PayloadType")))
        {
            PayloadTypePin->DefaultObject = PayloadStruct;
            Node->PinDefaultValueChanged(PayloadTypePin);
        }

        return Node;
    }

    bool ClearGraphNodes(UBlueprint* Blueprint, UEdGraph* Graph)
    {
        if (!Blueprint || !Graph)
        {
            return false;
        }

        TArray<UEdGraphNode*> Nodes = Graph->Nodes;
        for (UEdGraphNode* Node : Nodes)
        {
            if (Node)
            {
                FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
            }
        }
        return true;
    }

    UEdGraph* EnsureEventGraph(UBlueprint* Blueprint)
    {
        if (UEdGraph* Graph = ABT::Blueprint::FindGraph(Blueprint, TEXT("EventGraph")))
        {
            return Graph;
        }

        UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
            Blueprint,
            TEXT("EventGraph"),
            UEdGraph::StaticClass(),
            UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddUbergraphPage(Blueprint, NewGraph);
        return NewGraph;
    }
}

namespace ABT::Blueprint::Ops
{
    bool ConfigureButtonPagesWidget(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TArray<FString>& OutMessages, FString& OutError)
    {
        UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint);
        if (!WidgetBlueprint)
        {
            OutError = TEXT("configure_button_pages_widget requires a WidgetBlueprint target");
            return false;
        }
        if (!WidgetBlueprint->WidgetTree)
        {
            WidgetBlueprint->WidgetTree = NewObject<UWidgetTree>(WidgetBlueprint, TEXT("WidgetTree"), RF_Transactional);
        }

        UWidgetTree* Tree = WidgetBlueprint->WidgetTree;
        Tree->Modify();

        const TArray<TSharedPtr<FJsonObject>> Pages = ReadPageSpecs(Op);
        if (Pages.Num() > 10)
        {
            OutError = TEXT("configure_button_pages_widget supports at most 10 pages");
            return false;
        }

        const float PopupDuration = static_cast<float>(ABTJson::GetNumber(Op, TEXT("popupDuration"), 1.0));
        const float InitialOpacity = static_cast<float>(ABTJson::GetNumber(Op, TEXT("initialOpacity"), 0.5));
        const FVector2D InitialScale(
            static_cast<float>(ABTJson::GetNumber(Op, TEXT("initialScaleX"), 0.2)),
            static_cast<float>(ABTJson::GetNumber(Op, TEXT("initialScaleY"), 0.2)));

        UVerticalBox* Root = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Root"));
        UHorizontalBox* ButtonList = Tree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("ButtonList"));
        UOverlay* PageLayer = Tree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("PageLayer"));

        if (!Root || !ButtonList || !PageLayer)
        {
            OutError = TEXT("Failed to construct widget tree");
            return false;
        }

        Tree->RootWidget = Root;
        if (UVerticalBoxSlot* ButtonSlot = Root->AddChildToVerticalBox(ButtonList))
        {
            ButtonSlot->SetPadding(FMargin(16.0f, 16.0f, 16.0f, 8.0f));
            ButtonSlot->SetHorizontalAlignment(HAlign_Center);
        }
        if (UVerticalBoxSlot* PageSlot = Root->AddChildToVerticalBox(PageLayer))
        {
            PageSlot->SetPadding(FMargin(16.0f, 8.0f, 16.0f, 16.0f));
            PageSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        }

        for (int32 Index = 0; Index < Pages.Num(); ++Index)
        {
            const TSharedPtr<FJsonObject>& Page = Pages[Index];
            const FString ButtonText = ABTJson::GetString(Page, TEXT("buttonText"), FString::Printf(TEXT("Page %d"), Index + 1));
            const FString TitleText = ABTJson::GetString(Page, TEXT("title"), ButtonText);
            const FString BodyText = ABTJson::GetString(Page, TEXT("body"), TEXT(""));

            UButton* Button = Tree->ConstructWidget<UButton>(UButton::StaticClass(), *FString::Printf(TEXT("PageButton_%d"), Index));
            UTextBlock* ButtonLabel = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), *FString::Printf(TEXT("PageButtonLabel_%d"), Index));
            UBorder* PagePanel = Tree->ConstructWidget<UBorder>(UBorder::StaticClass(), *FString::Printf(TEXT("PagePanel_%d"), Index));
            UVerticalBox* PageContent = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), *FString::Printf(TEXT("PageContent_%d"), Index));
            UTextBlock* PageTitle = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), *FString::Printf(TEXT("PageTitle_%d"), Index));
            UTextBlock* PageBody = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), *FString::Printf(TEXT("PageBody_%d"), Index));

            if (!Button || !ButtonLabel || !PagePanel || !PageContent || !PageTitle || !PageBody)
            {
                OutError = TEXT("Failed to construct a button page widget");
                return false;
            }

            Button->bIsVariable = true;
            PagePanel->bIsVariable = true;

            ButtonLabel->SetText(FText::FromString(ButtonText));
            ButtonLabel->SetJustification(ETextJustify::Center);
            Button->AddChild(ButtonLabel);
            if (UHorizontalBoxSlot* ButtonBoxSlot = ButtonList->AddChildToHorizontalBox(Button))
            {
                ButtonBoxSlot->SetPadding(FMargin(4.0f, 0.0f));
            }

            PageTitle->SetText(FText::FromString(TitleText));
            PageTitle->SetJustification(ETextJustify::Center);
            PageBody->SetText(FText::FromString(BodyText));
            PageBody->SetAutoWrapText(true);
            PageBody->SetJustification(ETextJustify::Center);
            PageContent->AddChildToVerticalBox(PageTitle);
            PageContent->AddChildToVerticalBox(PageBody);

            PagePanel->SetPadding(FMargin(24.0f));
            PagePanel->SetVisibility(ESlateVisibility::Collapsed);
            PagePanel->SetRenderOpacity(InitialOpacity);
            FWidgetTransform Transform = GetWidgetRenderTransform(PagePanel);
            Transform.Scale = InitialScale;
            PagePanel->SetRenderTransform(Transform);
            PagePanel->AddChild(PageContent);

            if (UOverlaySlot* OverlaySlot = PageLayer->AddChildToOverlay(PagePanel))
            {
                OverlaySlot->SetHorizontalAlignment(HAlign_Center);
                OverlaySlot->SetVerticalAlignment(VAlign_Center);
                OverlaySlot->SetPadding(FMargin(24.0f));
            }

            ConfigurePagePopupAnimation(WidgetBlueprint, PagePanel, Index, PopupDuration, InitialOpacity, InitialScale);
        }

        EnsureWidgetGuids(WidgetBlueprint);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
        OutMessages.Add(FString::Printf(TEXT("Configured button pages widget with %d pages and UMG popup animations"), Pages.Num()));
        return true;
    }

    bool ConfigureFigmaWidget(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TArray<FString>& OutMessages, FString& OutError)
    {
        UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint);
        if (!WidgetBlueprint)
        {
            OutError = TEXT("configure_figma_widget requires a WidgetBlueprint target");
            return false;
        }
        if (!WidgetBlueprint->WidgetTree)
        {
            WidgetBlueprint->WidgetTree = NewObject<UWidgetTree>(WidgetBlueprint, TEXT("WidgetTree"), RF_Transactional);
        }

        UWidgetTree* Tree = WidgetBlueprint->WidgetTree;
        Tree->Modify();

        const TSharedPtr<FJsonObject>* RootSpecPtr = nullptr;
        TSharedPtr<FJsonObject> RootSpec;
        if (Op->TryGetObjectField(TEXT("root"), RootSpecPtr) && RootSpecPtr && RootSpecPtr->IsValid())
        {
            RootSpec = *RootSpecPtr;
        }
        else
        {
            RootSpec = ABTJson::Object();
            RootSpec->SetStringField(TEXT("type"), TEXT("CanvasPanel"));
            RootSpec->SetStringField(TEXT("name"), TEXT("RootCanvas"));

            const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
            if (Op->TryGetArrayField(TEXT("children"), Children) && Children)
            {
                RootSpec->SetArrayField(TEXT("children"), *Children);
            }
        }

        if (!RootSpec.IsValid())
        {
            OutError = TEXT("configure_figma_widget.root must be an object");
            return false;
        }

        int32 WidgetIndex = 0;
        UWidget* RootWidget = nullptr;
        if (!BuildFigmaWidgetTree(Tree, RootSpec, nullptr, WidgetIndex, RootWidget, OutError))
        {
            return false;
        }

        if (!RootWidget)
        {
            OutError = TEXT("configure_figma_widget failed to create a root widget");
            return false;
        }

        Tree->RootWidget = RootWidget;
        EnsureWidgetGuids(WidgetBlueprint);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

        TArray<UWidget*> AllWidgets;
        Tree->GetAllWidgets(AllWidgets);
        OutMessages.Add(FString::Printf(TEXT("Configured figma widget tree with %d widgets"), AllWidgets.Num()));
        return true;
    }

    bool ConfigureMessagePlateWidget(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Op, TArray<FString>& OutMessages, FString& OutError)
    {
        UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint);
        if (!WidgetBlueprint)
        {
            OutError = TEXT("configure_message_plate_widget requires a WidgetBlueprint target");
            return false;
        }

        UClass* ParentClass = LoadWidgetClassFromPath(ABTJson::GetString(Op, TEXT("parentClass"), TEXT("/Script/UMG.UserWidget")));
        if (!ParentClass || !ParentClass->IsChildOf(UUserWidget::StaticClass()))
        {
            OutError = TEXT("configure_message_plate_widget parentClass must derive from UserWidget");
            return false;
        }

        if (WidgetBlueprint->ParentClass != ParentClass)
        {
            WidgetBlueprint->Modify();
            WidgetBlueprint->ParentClass = ParentClass;
            FBlueprintEditorUtils::RefreshAllNodes(WidgetBlueprint);
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
            OutMessages.Add(FString::Printf(TEXT("Reparented widget to %s"), *ParentClass->GetPathName()));
        }

        UScriptStruct* PayloadStruct = LoadScriptStructFromPath(ABTJson::GetString(Op, TEXT("payloadStruct")));
        if (!PayloadStruct)
        {
            OutError = FString::Printf(TEXT("Payload struct not found: %s"), *ABTJson::GetString(Op, TEXT("payloadStruct")));
            return false;
        }

        UTexture2D* TrueTexture = RequireTexture(Op, TEXT("trueTexture"), OutError);
        if (!TrueTexture)
        {
            return false;
        }

        UTexture2D* FalseTexture = RequireTexture(Op, TEXT("falseTexture"), OutError);
        if (!FalseTexture)
        {
            return false;
        }

        if (!WidgetBlueprint->WidgetTree)
        {
            WidgetBlueprint->WidgetTree = NewObject<UWidgetTree>(WidgetBlueprint, TEXT("WidgetTree"), RF_Transactional);
        }

        UWidgetTree* Tree = WidgetBlueprint->WidgetTree;
        Tree->Modify();

        const FName BorderName(*ABTJson::GetString(Op, TEXT("backgroundName"), TEXT("Border_BG")));
        const FName TextName(*ABTJson::GetString(Op, TEXT("textName"), TEXT("LicensePlateText")));
        const FName AnimationName(*ABTJson::GetString(Op, TEXT("animationName"), TEXT("Show")));
        const FName ActionVariableName(*ABTJson::GetString(Op, TEXT("asyncActionVariable"), TEXT("LicensePlateMessageAction")));
        const FString StringField = ABTJson::GetString(Op, TEXT("stringField"), TEXT("StringValue"));
        const FString BoolField = ABTJson::GetString(Op, TEXT("boolField"), TEXT("BoolValue"));
        const FString Channel = ABTJson::GetString(Op, TEXT("channel"));
        const float AnimationDuration = static_cast<float>(ABTJson::GetNumber(Op, TEXT("duration"), 1.0));

        UBorder* Border = Cast<UBorder>(Tree->FindWidget(BorderName));
        if (!Border)
        {
            Border = Tree->ConstructWidget<UBorder>(UBorder::StaticClass(), BorderName);
        }
        UTextBlock* Text = Cast<UTextBlock>(Tree->FindWidget(TextName));
        if (!Text)
        {
            Text = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TextName);
        }

        if (!Border || !Text)
        {
            OutError = TEXT("Failed to construct license plate widgets");
            return false;
        }

        Border->bIsVariable = true;
        Border->SetBrushFromTexture(FalseTexture);
        Border->SetPadding(FMargin(0.0f));
        Border->SetRenderOpacity(0.0f);
        Border->SetContent(Text);

        if (UBorderSlot* TextSlot = Cast<UBorderSlot>(Text->Slot))
        {
            TextSlot->SetHorizontalAlignment(HAlign_Center);
            TextSlot->SetVerticalAlignment(VAlign_Center);
        }

        Text->bIsVariable = true;
        Text->SetText(FText::GetEmpty());
        Text->SetJustification(ETextJustify::Center);
        FSlateFontInfo Font = GetTextBlockFont(Text);
        Font.Size = ABTJson::GetInt(Op, TEXT("fontSize"), 28);
        Text->SetFont(Font);
        FLinearColor TextColor;
        if (TryReadColor(Op, TEXT("textColor"), TextColor) || TryReadColor(Op, TEXT("color"), TextColor))
        {
            Text->SetColorAndOpacity(FSlateColor(TextColor));
        }
        else
        {
            Text->SetColorAndOpacity(FSlateColor(FLinearColor::Black));
        }

        Tree->RootWidget = Border;

        UWidgetAnimation* ShowAnimation = FindOrCreateWidgetAnimation(WidgetBlueprint, AnimationName, AnimationDuration);
        if (!ShowAnimation || !ShowAnimation->MovieScene)
        {
            OutError = TEXT("Failed to create widget opacity animation");
            return false;
        }
        const FFrameNumber EndFrame = FrameAtSeconds(ShowAnimation->MovieScene, FMath::Max(AnimationDuration, 0.01f));
        const FGuid BorderBinding = ShowAnimation->MovieScene->AddPossessable(Border->GetName(), Border->GetClass());
        BindWidgetToAnimation(ShowAnimation, Border, BorderBinding);
        AddOpacityTrack(ShowAnimation->MovieScene, BorderBinding, 0.0f, EndFrame);
        EnsureWidgetGuids(WidgetBlueprint);

        UEdGraph* EventGraph = EnsureEventGraph(WidgetBlueprint);
        if (!EventGraph)
        {
            OutError = TEXT("Failed to find or create EventGraph");
            return false;
        }
        EventGraph->Modify();
        ClearGraphNodes(WidgetBlueprint, EventGraph);

        UClass* AsyncActionClass = LoadObject<UClass>(nullptr, TEXT("/Script/GameplayMessageRuntime.AsyncAction_ListenForGameplayMessage"));
        if (!EnsureAsyncActionVariable(WidgetBlueprint, ActionVariableName, AsyncActionClass, OutMessages))
        {
            OutError = TEXT("Failed to ensure async action variable");
            return false;
        }

        UK2Node_Event* ConstructEvent = AddWidgetEventNode(EventGraph, TEXT("Construct"), 0, 0);
        UK2Node_Event* DestructEvent = AddWidgetEventNode(EventGraph, TEXT("Destruct"), 0, 420);
        UK2Node_AsyncAction* ListenNode = AddListenForGameplayMessagesNode(EventGraph, PayloadStruct, Channel, 300, 0, OutError);
        if (!ListenNode)
        {
            return false;
        }

        UK2Node_VariableSet* SetActionNode = AddVariableSetNode(EventGraph, ActionVariableName, 760, 80);
        UK2Node_BreakStruct* BreakPayloadNode = AddTypedNode<UK2Node_BreakStruct>(EventGraph, 760, 300);
        BreakPayloadNode->StructType = PayloadStruct;
        BreakPayloadNode->AllocateDefaultPins();

        UK2Node_VariableGet* GetTextNode = AddVariableGetNode(EventGraph, TextName, 980, 470);
        UK2Node_CallFunction* StringToTextNode = AddCallFunctionNode(EventGraph, RequireFunction(UKismetTextLibrary::StaticClass(), TEXT("Conv_StringToText"), OutError), 980, 310);
        if (!StringToTextNode)
        {
            return false;
        }
        UK2Node_CallFunction* SetTextNode = AddCallFunctionNode(EventGraph, RequireFunction(UTextBlock::StaticClass(), TEXT("SetText"), OutError), 1240, 240);
        if (!SetTextNode)
        {
            return false;
        }

        UK2Node_IfThenElse* BranchNode = AddTypedNode<UK2Node_IfThenElse>(EventGraph, 1500, 240);
        BranchNode->AllocateDefaultPins();

        UK2Node_VariableGet* GetBorderForTrueNode = AddVariableGetNode(EventGraph, BorderName, 1700, 360);
        UK2Node_CallFunction* SetTrueTextureNode = AddCallFunctionNode(EventGraph, RequireFunction(UBorder::StaticClass(), TEXT("SetBrushFromTexture"), OutError), 1900, 160);
        if (!SetTrueTextureNode || !SetObjectPinDefault(SetTrueTextureNode, TEXT("Texture"), TrueTexture, OutError))
        {
            return false;
        }
        UK2Node_VariableGet* GetShowForTrueNode = AddVariableGetNode(EventGraph, AnimationName, 2140, 340);
        UK2Node_CallFunction* PlayTrueAnimationNode = AddCallFunctionNode(EventGraph, RequireFunction(UUserWidget::StaticClass(), TEXT("PlayAnimation"), OutError), 2320, 160);
        if (!PlayTrueAnimationNode)
        {
            return false;
        }

        UK2Node_VariableGet* GetBorderForFalseNode = AddVariableGetNode(EventGraph, BorderName, 1700, 720);
        UK2Node_CallFunction* SetFalseTextureNode = AddCallFunctionNode(EventGraph, RequireFunction(UBorder::StaticClass(), TEXT("SetBrushFromTexture"), OutError), 1900, 520);
        if (!SetFalseTextureNode || !SetObjectPinDefault(SetFalseTextureNode, TEXT("Texture"), FalseTexture, OutError))
        {
            return false;
        }
        UK2Node_VariableGet* GetShowForFalseNode = AddVariableGetNode(EventGraph, AnimationName, 2140, 700);
        UK2Node_CallFunction* PlayFalseAnimationNode = AddCallFunctionNode(EventGraph, RequireFunction(UUserWidget::StaticClass(), TEXT("PlayAnimation"), OutError), 2320, 520);
        if (!PlayFalseAnimationNode)
        {
            return false;
        }

        UK2Node_VariableGet* GetActionNode = AddVariableGetNode(EventGraph, ActionVariableName, 300, 640);
        UK2Node_CallFunction* CancelNode = AddCallFunctionNode(EventGraph, RequireFunction(AsyncActionClass, TEXT("Cancel"), OutError), 560, 480);
        if (!CancelNode)
        {
            return false;
        }

        if (!ConnectNodePins(ConstructEvent, TEXT("then"), ListenNode, TEXT("execute"), OutError) ||
            !ConnectNodePins(ListenNode, TEXT("then"), SetActionNode, TEXT("execute"), OutError) ||
            !ConnectNodePins(ListenNode, TEXT("AsyncTaskProxy"), SetActionNode, ActionVariableName.ToString(), OutError) ||
            !ConnectNodePins(ListenNode, TEXT("Payload"), BreakPayloadNode, PayloadStruct->GetName(), OutError) ||
            !ConnectNodePins(ListenNode, TEXT("OnMessageReceived"), SetTextNode, TEXT("execute"), OutError) ||
            !ConnectNodePins(BreakPayloadNode, StringField, StringToTextNode, TEXT("InString"), OutError) ||
            !ConnectNodePins(StringToTextNode, TEXT("ReturnValue"), SetTextNode, TEXT("InText"), OutError) ||
            !ConnectNodePins(GetTextNode, TextName.ToString(), SetTextNode, TEXT("self"), OutError) ||
            !ConnectNodePins(SetTextNode, TEXT("then"), BranchNode, TEXT("execute"), OutError) ||
            !ConnectNodePins(BreakPayloadNode, BoolField, BranchNode, TEXT("Condition"), OutError) ||
            !ConnectNodePins(BranchNode, TEXT("then"), SetTrueTextureNode, TEXT("execute"), OutError) ||
            !ConnectNodePins(GetBorderForTrueNode, BorderName.ToString(), SetTrueTextureNode, TEXT("self"), OutError) ||
            !ConnectNodePins(SetTrueTextureNode, TEXT("then"), PlayTrueAnimationNode, TEXT("execute"), OutError) ||
            !ConnectNodePins(GetShowForTrueNode, AnimationName.ToString(), PlayTrueAnimationNode, TEXT("InAnimation"), OutError) ||
            !ConnectNodePins(BranchNode, TEXT("else"), SetFalseTextureNode, TEXT("execute"), OutError) ||
            !ConnectNodePins(GetBorderForFalseNode, BorderName.ToString(), SetFalseTextureNode, TEXT("self"), OutError) ||
            !ConnectNodePins(SetFalseTextureNode, TEXT("then"), PlayFalseAnimationNode, TEXT("execute"), OutError) ||
            !ConnectNodePins(GetShowForFalseNode, AnimationName.ToString(), PlayFalseAnimationNode, TEXT("InAnimation"), OutError) ||
            !ConnectNodePins(DestructEvent, TEXT("then"), CancelNode, TEXT("execute"), OutError) ||
            !ConnectNodePins(GetActionNode, ActionVariableName.ToString(), CancelNode, TEXT("self"), OutError))
        {
            return false;
        }

        EnsureWidgetGuids(WidgetBlueprint);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
        OutMessages.Add(FString::Printf(TEXT("Configured message plate widget for %s with %s and 1 opacity animation"), *Channel, *PayloadStruct->GetName()));
        return true;
    }
}
