#pragma once

#include "CoreMinimal.h"

/**
 * The screen shown while a map loads.
 *
 * Source puts up a loading dialog with the game's picture on it, a progress bar and a line saying what it is
 * doing (CLoadingDialog, engine/vgui_baseui_interface.cpp). This is that: the mod's own logo out of its materials
 * folder, a bar fed by FLambdaLoadProgress, and the caption underneath.
 *
 * It goes through Unreal's movie player, which draws it on its own thread. That matters: the map load blocks the
 * game thread solid, so anything drawn the ordinary way would freeze on whatever frame it started with.
 */
class LAMBDAENGINE_API FLambdaLoadingScreen
{
public:
	/**
	 * Arms the loading screen for the next level load. The movie player throws its attributes away after every
	 * screen it shows, so this has to be called again before each load - once when the module starts, for the
	 * one that covers the game's own startup, and again before every travel the game asks for.
	 */
	static void Arm();
};
