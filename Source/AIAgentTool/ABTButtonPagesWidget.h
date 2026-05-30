// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Blueprint/UserWidget.h"
#include "ABTButtonPagesWidget.generated.h"

class UButton;
class UWidget;
class UWidgetAnimation;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FABTPopupMidpointEvent, int32, PageIndex);

UCLASS(Blueprintable)
class AIAGENTTOOL_API UABTButtonPagesWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AgentBlueprintTools|Pages")
	float PopupDuration = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AgentBlueprintTools|Pages")
	float InitialOpacity = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AgentBlueprintTools|Pages")
	FVector2D InitialScale = FVector2D(0.2f, 0.2f);

	UPROPERTY(BlueprintReadOnly, Category = "AgentBlueprintTools|Pages")
	int32 ActivePageIndex = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category = "AgentBlueprintTools|Pages")
	int32 LastMidpointPageIndex = INDEX_NONE;

	UPROPERTY(BlueprintAssignable, Category = "AgentBlueprintTools|Pages")
	FABTPopupMidpointEvent OnPopupMidpoint;

	UFUNCTION(BlueprintCallable, Category = "AgentBlueprintTools|Pages")
	void ShowPageByIndex(int32 PageIndex);

	UFUNCTION(BlueprintCallable, Category = "AgentBlueprintTools|Pages")
	void OnPopupAnimationMidpoint();

protected:
	virtual void NativeConstruct() override;

private:
	UPROPERTY(Transient)
	TArray<UWidget*> PageWidgets;

	UPROPERTY(Transient)
	UWidget* ActivePage;

	void CachePages();
	void BindButton(int32 Index);
	UWidgetAnimation* FindPopupAnimation(int32 PageIndex) const;

	UFUNCTION()
	void OnPageButton0Clicked();
	UFUNCTION()
	void OnPageButton1Clicked();
	UFUNCTION()
	void OnPageButton2Clicked();
	UFUNCTION()
	void OnPageButton3Clicked();
	UFUNCTION()
	void OnPageButton4Clicked();
	UFUNCTION()
	void OnPageButton5Clicked();
	UFUNCTION()
	void OnPageButton6Clicked();
	UFUNCTION()
	void OnPageButton7Clicked();
	UFUNCTION()
	void OnPageButton8Clicked();
	UFUNCTION()
	void OnPageButton9Clicked();
};
