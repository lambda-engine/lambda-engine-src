#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "LambdaHUD.generated.h"

/**
 * The player HUD: health, suit armour, ammo and a crosshair, laid out like Half-Life 2's (health bottom-left,
 * ammo bottom-right). Drawn with Canvas so it needs no UMG assets - a modder gets a working HUD with no editor work.
 */
UCLASS()
class LAMBDAENGINE_API ALambdaHUD : public AHUD
{
	GENERATED_BODY()

public:
	ALambdaHUD();

	virtual void DrawHUD() override;

protected:
	void DrawCrosshair(float CenterX, float CenterY);
	void DrawPanel(float X, float Y, float W, float H);
	void DrawLabelledValue(float X, float Y, const FString& Label, const FString& Value, const FLinearColor& Colour, float ValueScale);

	/** HL2's HUD amber. */
	FLinearColor HudColour = FLinearColor(1.0f, 0.68f, 0.28f, 1.0f);
	FLinearColor PanelColour = FLinearColor(0.05f, 0.05f, 0.05f, 0.5f);
	FLinearColor LowColour = FLinearColor(1.0f, 0.25f, 0.15f, 1.0f);

	UPROPERTY()
	TObjectPtr<UFont> HudFont;

	/** cl_showfps-style counter, smoothed so it reads steadily. */
	float SmoothedFrameTime = 0.0f;
};
