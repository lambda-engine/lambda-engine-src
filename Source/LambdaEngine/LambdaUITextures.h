#pragma once

#include "CoreMinimal.h"

class UTexture2D;

/**
 * Pictures the UI needs, loaded straight from the game directory.
 *
 * The material library is per-world and wants a BSP loaded before it will hand anything out, which is no use to
 * the main menu or the loading screen - neither of them has a world. This is the same VMT -> $basetexture -> VTF
 * walk done standing on its own, and what comes back is rooted and kept, because both of those are drawn on
 * every map load and the loading screen is drawn from a thread that must not be looking at a texture the garbage
 * collector is in the middle of taking away.
 */
class LAMBDAENGINE_API FLambdaUITextures
{
public:
	/**
	 * The texture a material names, e.g. "console/background01_widescreen" for materials/console/... .vmt.
	 * Null if the game directory has not got it. Looked up once; the answer, including "no", is remembered.
	 */
	static UTexture2D* Get(const FString& MaterialName);
};
