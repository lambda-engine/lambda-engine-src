#pragma once

#include "CoreMinimal.h"

class UFont;

/**
 * The faces the UI is written in.
 *
 * ClientScheme's CustomFontFiles: the scheme ships the face it wants next to the .res that names it, and Black
 * Mesa's HUD is all "Alte DIN 1451 Mittelschrift". They are read out of the game directory's resource folder
 * like any other piece of game content, so a mod that drops its own face in gets its own look.
 *
 * Kept here rather than on the HUD because the loading screen needs them too, and because a face rebuilt on every
 * map load would be a face rasterised again on every map load. Which face fills which role is fixed here for
 * now; it belongs in the scheme file eventually, the way Source keeps it.
 */
class LAMBDAENGINE_API FLambdaFonts
{
public:
	/**
	 * A face from the game directory, e.g. "din1451alt.ttf" for resource/din1451alt.ttf. Null if it is not
	 * there. Runtime-cached, so it can be rasterised at whatever size is asked for rather than magnified from a
	 * baked one. Looked up once - the answer, including "not there", is remembered.
	 */
	static UFont* Get(const FString& FileName);

	/** The HUD's numbers and the console: the scheme's own face. */
	static UFont* GetSchemeFont();
	/** The game's name on the main menu and the loading screen. */
	static UFont* GetTitleFont();
	/** The menu's items. */
	static UFont* GetMenuFont();
};
