#pragma once

#include "CoreMinimal.h"

class UFont;
class UTexture2D;

/**
 * How the main menu's title is drawn, as the game directory describes it.
 *
 * Source keeps this in two places and this reads both of them, by the same names and the same keys, so a mod
 * that has done it for Source has already done it here:
 *
 *   gameinfo.txt              "title" / "title2" - the two lines of text, and "gamelogo" for the picture
 *   resource/clientscheme.res "BaseSettings/Main.Title1.X|.Y|.Color" and the same for Title2,
 *                             "Fonts/ClientTitleFont" for the face they are written in, and
 *                             "CustomFontFiles" for the faces the mod ships
 *
 * A key the game directory does not set keeps the value below, which is what the menu looked like before any
 * of this was readable - so a mod that says nothing looks exactly as it did.
 *
 * The positions are in the 640x480 screen the rest of the menu is laid out against (LambdaHUD.cpp scales
 * everything by the real height over 480). Source's own numbers are nominally the same screen, so they carry
 * over; what Source does with them after that is its own proportional scaling, which this engine does not copy.
 */
struct LAMBDAENGINE_API FLambdaMenuScheme
{
	/** The first line: what the mod calls itself, falling back to gameinfo's "game" and then to the engine. */
	FString Title1;
	/** The second line, for a mod that wants one. Empty is not drawn. */
	FString Title2;

	FVector2D Title1Position = FVector2D(32.0f, 48.0f);
	FLinearColor Title1Colour = FLinearColor::White;

	FVector2D Title2Position = FVector2D(32.0f, 96.0f);
	FLinearColor Title2Colour = FLinearColor::White;

	/** ClientTitleFont: the face, how tall it is drawn, and whether it is drawn bold and added to what is behind. */
	FString TitleFontName;
	float TitleHeight = 44.0f;
	int32 TitleWeight = 0;
	bool bTitleAdditive = false;

	/** Every face CustomFontFiles names, by the name the scheme calls it: name -> "resource/whatever.ttf". */
	TMap<FString, FString> CustomFontFiles;

	/**
	 * The face the titles are written in: whatever ClientTitleFont names, and the engine's own when the mod
	 * has not named one or nothing in the game directory answers to the name.
	 */
	UFont* GetTitleFont() const;

	/** Read once, the first time the menu is drawn, and kept - like every other piece of the UI. */
	static const FLambdaMenuScheme& Get();
};

/**
 * The picture over the menu items, as resource/gamelogo.res describes it - Source's GameLogo panel.
 *
 * Two rectangles: GameLogo is the area the logo is allowed to cover, and Logo is where the picture sits inside
 * that area and how big it is drawn. A picture larger than the area is cropped by it, which is how Source packs
 * a logo into a power-of-two texture and shows only the part that matters.
 *
 * Both are in the same 640x480 screen as the rest of the menu.
 */
struct LAMBDAENGINE_API FLambdaGameLogo
{
	/** gameinfo.txt's "gamelogo", and gamelogo.res's own "Logo/visible", both of which have to say yes. */
	bool bVisible = false;

	/** Logo/image: materials/vgui/<this>. */
	FString ImageName = TEXT("logo");

	/** GameLogo/offsetX and offsetY: where the area sits, relative to the top of the menu items. */
	FVector2D Offset = FVector2D::ZeroVector;
	/** GameLogo/wide and tall: how much of the picture is shown. */
	FVector2D PanelSize = FVector2D::ZeroVector;

	/** Logo/xpos and ypos: where the picture sits inside the area, usually 0,0. */
	FVector2D ImagePosition = FVector2D::ZeroVector;
	/** Logo/wide and tall: how big the whole picture is drawn. */
	FVector2D ImageSize = FVector2D::ZeroVector;

	UTexture2D* GetTexture() const;

	static const FLambdaGameLogo& Get();
};
