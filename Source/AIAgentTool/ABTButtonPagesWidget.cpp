// Copyright Epic Games, Inc. All Rights Reserved.

#include "ABTButtonPagesWidget.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/Button.h"
#include "Components/Widget.h"

namespace
{
	constexpr int32 MaxAutoBoundPages = 10;

	FName ButtonName(int32 Index)
	{
		return *FString::Printf(TEXT("PageButton_%d"), Index);
	}

	FName PageName(int32 Index)
	{
		return *FString::Printf(TEXT("PagePanel_%d"), Index);
	}

	FName PopupAnimationName(int32 Index)
	{
		return *FString::Printf(TEXT("PagePopup_%d"), Index);
	}
}

void UABTButtonPagesWidget::NativeConstruct()
{
	Super::NativeConstruct();

	CachePages();
	for (int32 Index = 0; Index < MaxAutoBoundPages; ++Index)
	{
		BindButton(Index);
	}
}

void UABTButtonPagesWidget::CachePages()
{
	PageWidgets.Reset();
	if (!WidgetTree)
	{
		return;
	}

	for (int32 Index = 0; Index < MaxAutoBoundPages; ++Index)
	{
		if (UWidget* Page = WidgetTree->FindWidget(PageName(Index)))
		{
			Page->SetVisibility(ESlateVisibility::Collapsed);
			PageWidgets.Add(Page);
		}
	}
}

void UABTButtonPagesWidget::BindButton(int32 Index)
{
	if (!WidgetTree)
	{
		return;
	}

	UButton* Button = Cast<UButton>(WidgetTree->FindWidget(ButtonName(Index)));
	if (!Button)
	{
		return;
	}

	Button->OnClicked.Clear();
	switch (Index)
	{
	case 0: Button->OnClicked.AddDynamic(this, &UABTButtonPagesWidget::OnPageButton0Clicked); break;
	case 1: Button->OnClicked.AddDynamic(this, &UABTButtonPagesWidget::OnPageButton1Clicked); break;
	case 2: Button->OnClicked.AddDynamic(this, &UABTButtonPagesWidget::OnPageButton2Clicked); break;
	case 3: Button->OnClicked.AddDynamic(this, &UABTButtonPagesWidget::OnPageButton3Clicked); break;
	case 4: Button->OnClicked.AddDynamic(this, &UABTButtonPagesWidget::OnPageButton4Clicked); break;
	case 5: Button->OnClicked.AddDynamic(this, &UABTButtonPagesWidget::OnPageButton5Clicked); break;
	case 6: Button->OnClicked.AddDynamic(this, &UABTButtonPagesWidget::OnPageButton6Clicked); break;
	case 7: Button->OnClicked.AddDynamic(this, &UABTButtonPagesWidget::OnPageButton7Clicked); break;
	case 8: Button->OnClicked.AddDynamic(this, &UABTButtonPagesWidget::OnPageButton8Clicked); break;
	case 9: Button->OnClicked.AddDynamic(this, &UABTButtonPagesWidget::OnPageButton9Clicked); break;
	default: break;
	}
}

UWidgetAnimation* UABTButtonPagesWidget::FindPopupAnimation(int32 PageIndex) const
{
	const FName TargetName = PopupAnimationName(PageIndex);
	for (UClass* Class = GetClass(); Class; Class = Class->GetSuperClass())
	{
		const UWidgetBlueprintGeneratedClass* WidgetClass = Cast<UWidgetBlueprintGeneratedClass>(Class);
		if (!WidgetClass)
		{
			continue;
		}

		for (UWidgetAnimation* Animation : WidgetClass->Animations)
		{
			if (Animation && Animation->GetFName() == TargetName)
			{
				return Animation;
			}
		}
	}
	return nullptr;
}

void UABTButtonPagesWidget::ShowPageByIndex(int32 PageIndex)
{
	if (!PageWidgets.IsValidIndex(PageIndex))
	{
		return;
	}

	for (UWidget* Page : PageWidgets)
	{
		if (Page)
		{
			Page->SetVisibility(ESlateVisibility::Collapsed);
		}
	}

	ActivePage = PageWidgets[PageIndex];
	if (!ActivePage)
	{
		return;
	}

	ActivePageIndex = PageIndex;
	ActivePage->SetRenderOpacity(InitialOpacity);
	FWidgetTransform Transform = ActivePage->RenderTransform;
	Transform.Scale = InitialScale;
	ActivePage->SetRenderTransform(Transform);
	ActivePage->SetVisibility(ESlateVisibility::Visible);

	if (UWidgetAnimation* PopupAnimation = FindPopupAnimation(PageIndex))
	{
		const float PlaybackSpeed = PopupDuration > 0.0f ? 1.0f / PopupDuration : 1.0f;
		PlayAnimation(PopupAnimation, 0.0f, 1, EUMGSequencePlayMode::Forward, PlaybackSpeed, false);
	}
	else
	{
		ActivePage->SetRenderOpacity(1.0f);
		Transform.Scale = FVector2D(1.0f, 1.0f);
		ActivePage->SetRenderTransform(Transform);
	}
}

void UABTButtonPagesWidget::OnPopupAnimationMidpoint()
{
	LastMidpointPageIndex = ActivePageIndex;
	OnPopupMidpoint.Broadcast(ActivePageIndex);
}

void UABTButtonPagesWidget::OnPageButton0Clicked() { ShowPageByIndex(0); }
void UABTButtonPagesWidget::OnPageButton1Clicked() { ShowPageByIndex(1); }
void UABTButtonPagesWidget::OnPageButton2Clicked() { ShowPageByIndex(2); }
void UABTButtonPagesWidget::OnPageButton3Clicked() { ShowPageByIndex(3); }
void UABTButtonPagesWidget::OnPageButton4Clicked() { ShowPageByIndex(4); }
void UABTButtonPagesWidget::OnPageButton5Clicked() { ShowPageByIndex(5); }
void UABTButtonPagesWidget::OnPageButton6Clicked() { ShowPageByIndex(6); }
void UABTButtonPagesWidget::OnPageButton7Clicked() { ShowPageByIndex(7); }
void UABTButtonPagesWidget::OnPageButton8Clicked() { ShowPageByIndex(8); }
void UABTButtonPagesWidget::OnPageButton9Clicked() { ShowPageByIndex(9); }
