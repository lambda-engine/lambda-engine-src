#pragma once

#include "CoreMinimal.h"

class UFont;

/**
 * The faces the UI is written in.
 *
 * ClientScheme's CustomFontFiles: the scheme ships the face it wants next to the .res that names it, and Black
 * Mesa's HUD is all "Alte DIN 1451 Mittelschrift". It is read out of the game directory like any other piece of
 * game content, so a mod that drops its own face in gets its own look.
 *
 * Kept here rather than on the HUD because the loading screen needs it too, and because a face rebuilt on every
 * map load would be a face rasterised again on every map load.
 */
class LAMBDAENGINE_API FLambdaFonts
{
public:
	/**
	 * The scheme's own face, or null if the game directory has not got one. It is a runtime-cached font, so it
	 * can be rasterised at whatever size is asked for rather than magnified from a baked one.
	 */
	static UFont* GetSchemeFont();
};
