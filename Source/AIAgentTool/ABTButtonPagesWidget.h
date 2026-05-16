// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Blueprint/UserWidget.h"
#include "ABTButtonPagesWidget.generated.h"

class UButton;
class UWidget;

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

	UFUNCTION(BlueprintCallable, Category = "AgentBlueprintTools|Pages")
	void ShowPageByIndex(int32 PageIndex);

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

private:
	UPROPERTY(Transient)
	TArray<TObjectPtr<UWidget>> PageWidgets;

	UPROPERTY(Transient)
	TObjectPtr<UWidget> ActivePage;

	FTimerHandle PopupTimerHandle;
	double PopupStartTime = 0.0;

	void CachePages();
	void BindButton(int32 Index);
	void AdvancePopup();

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
