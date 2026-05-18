#include "Blueprint/Ops/ABTWidgetBlueprintOps.h"
#include "Utils/ABTJson.h"

#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/Widget.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MovieScene.h"
#include "Sections/MovieSceneEventTriggerSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "WidgetBlueprint.h"

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

        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
        OutMessages.Add(FString::Printf(TEXT("Configured button pages widget with %d pages and UMG popup animations"), Pages.Num()));
        return true;
    }
}
