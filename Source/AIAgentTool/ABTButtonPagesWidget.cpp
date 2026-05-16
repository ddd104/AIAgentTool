// Copyright Epic Games, Inc. All Rights Reserved.

#include "ABTButtonPagesWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/Widget.h"
#include "Engine/World.h"
#include "TimerManager.h"

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

void UABTButtonPagesWidget::NativeDestruct()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(PopupTimerHandle);
	}

	Super::NativeDestruct();
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

	ActivePage->SetRenderOpacity(InitialOpacity);
	FWidgetTransform Transform = ActivePage->GetRenderTransform();
	Transform.Scale = InitialScale;
	ActivePage->SetRenderTransform(Transform);
	ActivePage->SetVisibility(ESlateVisibility::Visible);

	if (UWorld* World = GetWorld())
	{
		PopupStartTime = World->GetTimeSeconds();
		World->GetTimerManager().SetTimer(PopupTimerHandle, this, &UABTButtonPagesWidget::AdvancePopup, 1.0f / 60.0f, true);
	}
}

void UABTButtonPagesWidget::AdvancePopup()
{
	UWorld* World = GetWorld();
	if (!World || !ActivePage)
	{
		return;
	}

	const float Duration = FMath::Max(PopupDuration, 0.01f);
	const float Alpha = FMath::Clamp(static_cast<float>((World->GetTimeSeconds() - PopupStartTime) / Duration), 0.0f, 1.0f);
	const float Eased = FMath::InterpEaseOut(0.0f, 1.0f, Alpha, 2.0f);

	ActivePage->SetRenderOpacity(FMath::Lerp(InitialOpacity, 1.0f, Eased));
	FWidgetTransform Transform = ActivePage->GetRenderTransform();
	Transform.Scale = FMath::Lerp(InitialScale, FVector2D(1.0f, 1.0f), Eased);
	ActivePage->SetRenderTransform(Transform);

	if (Alpha >= 1.0f)
	{
		World->GetTimerManager().ClearTimer(PopupTimerHandle);
	}
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
