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
	/** Source's main menu: the game's name, and what you can do about it, down the bottom left. */
	void DrawMainMenu(class ULambdaMainMenu* Menu, float W, float H);
	/** The face the menu and console are written in: the scheme's own, which is a real font and scales cleanly. */
	UFont* UIFont() const;
	/**
	 * Draws text with the glyphs rasterised at the height asked for.
	 *
	 * DrawText's scale is not a size: Canvas rasterises a runtime font once at its LegacyFontSize and then
	 * magnifies the result (FCanvasSimpleTextItem::DrawStringInternal_RuntimeCache passes only the DPI scale to
	 * the font cache), so anything drawn bigger than 31px was a blown-up bitmap. This asks for the size instead.
	 */
	void DrawTextAtHeight(const FString& Text, const FLinearColor& Colour, float X, float Y, UFont* Font, float PixelHeight);
	/** What DrawTextAtHeight would take up, for laying out and for knowing what the mouse is over. */
	FVector2D MeasureTextAtHeight(const FString& Text, UFont* Font, float PixelHeight) const;
	/** Where the mouse was last frame, so hovering only takes the selection when the mouse actually moves. */
	FVector2D LastMenuMouse = FVector2D::ZeroVector;
	/** Source's console: a panel over the top of the screen with what it has said, and the line being typed. */
	void DrawConsole(class ULambdaConsole* Console, float W, float H);
	void DrawPanel(float X, float Y, float W, float H);
	void DrawLabelledValue(float X, float Y, const FString& Label, const FString& Value, const FLinearColor& Colour, float ValueScale);

	/**
	 * A Black Mesa number field: the value in a fixed number of digits, with the leading zeros left in place and
	 * drawn in the dim colour, so "13" of a 3-digit field reads as a faint 0 followed by a bright 13. Returns the
	 * x it started drawing at.
	 */
	float DrawNumberField(float RightX, float Y, int32 Value, int32 Digits, float PixelHeight,
		const FLinearColor& Bright, const FLinearColor& Dim);
	/** Maps an HL2 ammo pool name onto the Black Mesa icon for that round. */
	static FString AmmoIconName(const FString& AmmoType);
	/** Draws one of Black Mesa's HUD icons (materials/vgui/hud) tinted, sized in pixels. */
	void DrawHudIcon(const FString& TextureName, float X, float Y, float Size, const FLinearColor& Colour);
	class ULambdaMaterialLibrary* GetMaterials();

	/** HL2's HUD amber. */
	// Black Mesa's scheme is one amber - "Orange" "255 176 0" - used at a handful of alphas, and one red for
	// trouble. The dim tiers are the same hue faded, which is what makes a field's leading zeros sink into the
	// background instead of reading as another colour.
	static constexpr float BMAmberG = 176.0f / 255.0f;
	FLinearColor HudColour = FLinearColor(1.0f, BMAmberG, 0.0f, 1.0f);			// OrangeBright
	FLinearColor HudColourNormal = FLinearColor(1.0f, BMAmberG, 0.0f, 160.0f / 255.0f);	// Orange
	FLinearColor HudColourDim = FLinearColor(1.0f, BMAmberG, 0.0f, 110.0f / 255.0f);	// OrangeDim
	FLinearColor HudColourDark = FLinearColor(1.0f, BMAmberG, 0.0f, 42.0f / 255.0f);	// OrangeDark
	FLinearColor PanelColour = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);			// BM paints no panel behind the HUD
	FLinearColor LowColour = FLinearColor(1.0f, 28.0f / 255.0f, 0.0f, 1.0f);		// RedBright
	FLinearColor LowColourDark = FLinearColor(1.0f, 28.0f / 255.0f, 0.0f, 42.0f / 255.0f);	// RedDark

	UPROPERTY()
	TObjectPtr<UFont> HudFont;
	/** Black Mesa's own face, loaded from the game directory at startup; null falls back to the engine font. */
	UPROPERTY(Transient)
	TObjectPtr<UFont> SchemeFont;
	void LoadSchemeFont();
	UPROPERTY(Transient)
	TWeakObjectPtr<class ULambdaMaterialLibrary> Materials;

	/** cl_showfps-style counter, smoothed so it reads steadily. */
	float SmoothedFrameTime = 0.0f;

	/** hud_health: a brief brighten when hurt, a steady pulse when low (the HudAnimations "HealthLow" loop). */
	float LastHealthSeen = -1.0f;
	float DamageFlashEndTime = 0.0f;
	/** hud_ammo: the panel brightens when the weapon changes. */
	TWeakObjectPtr<class ALambdaWeapon> LastWeaponSeen;
	float WeaponFlashEndTime = 0.0f;

	void DrawDamageIndicator(class ALambdaCharacter* Player, float W, float H, float Now);
	void DrawWeaponSelection(class ALambdaCharacter* Player, float W, float Now);
	void DrawPickupHistory(class ALambdaCharacter* Player, float W, float H, float Now);
};
