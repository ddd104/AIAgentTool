#include "Blueprint/Ops/ABTWidgetBlueprintOps.h"
#include "Utils/ABTJson.h"

#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
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
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Paths.h"
#include "MovieScene.h"
#include "Sections/MovieSceneEventTriggerSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Styling/SlateBrush.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "WidgetBlueprint.h"

#include <initializer_list>

namespace
{
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
            WidgetBlueprint->OnVariableAdded(AnimationName);
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
        Section->GetChannel().AddLinearKey(FFrameNumber(0), InitialOpacity);
        Section->GetChannel().AddLinearKey(EndFrame, 1.0f);
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

        UMovieSceneEventTrack* EventTrack = MovieScene->AddTrack<UMovieSceneEventTrack>();
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
        if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
        {
            return;
        }

        TArray<UWidget*> AllWidgets;
        WidgetBlueprint->WidgetTree->GetAllWidgets(AllWidgets);
        for (const UWidget* Widget : AllWidgets)
        {
            if (Widget && !WidgetBlueprint->WidgetVariableNameToGuidMap.Contains(Widget->GetFName()))
            {
                WidgetBlueprint->OnVariableAdded(Widget->GetFName());
            }
        }
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
            FWidgetTransform Transform = Widget->GetRenderTransform();
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
                FSlateFontInfo Font = TextBlock->GetFont();
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
            FWidgetTransform Transform = PagePanel->GetRenderTransform();
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
}
