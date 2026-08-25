#pragma once

#include "CoreMinimal.h"

/**
 * The stages a map load goes through, as the loading screen reports them.
 *
 * Source drives its loading dialog from named progress points - PROGRESS_LOADWORLDMODEL, PROGRESS_LEVELINIT,
 * PROGRESS_PRECACHE and the rest - and each one both moves the bar and changes the line of text under it
 * (engine/vgui_baseui_interface.cpp, CLoadingDialog::SetProgressPoint). These are the same idea, cut down to the
 * stages this engine actually has.
 */
enum class ELambdaLoadStage : uint8
{
	Idle,
	ReadingMap,			// PROGRESS_LOADWORLDMODEL: pulling the BSP off disk and parsing its lumps
	BuildingWorld,		// turning the faces into meshes and the texdata into materials
	SpawningEntities,	// PROGRESS_LEVELINIT: MapEntity_ParseAllEntities, which is where the models get loaded
	Precaching,			// PROGRESS_PRECACHE: decals and impact sounds
	Done,
};

/**
 * How far the map load has got.
 *
 * The loading screen is drawn on the movie player's own thread precisely so it can animate while the game thread
 * is stuck inside the load, so this is written from one thread and read from another. Everything goes through the
 * one lock; it is read once a frame by a single widget, so the cost of that is nothing.
 */
class LAMBDASOURCE_API FLambdaLoadProgress
{
public:
	/** A map load is starting. Resets the bar to nothing and names what is being loaded. */
	static void Begin(const FString& MapName);
	/** Moves to the next stage, which sets the bar to that stage's starting point and changes the caption. */
	static void SetStage(ELambdaLoadStage Stage);
	/**
	 * Moves the bar within the current stage. Fraction is 0..1 through that stage's own work - two hundred
	 * entities spawned out of five hundred, say - and is scaled into the slice of the bar the stage owns.
	 */
	static void SetStageFraction(float Fraction);
	/** The load has finished, one way or the other. */
	static void End();

	/** 0..1 across the whole load, or 0 if nothing has reported anything - see IsMeasured. */
	static float GetFraction();
	/** False while nothing has reported a stage, which is when the bar should just sweep rather than fill. */
	static bool IsMeasured();
	/** The caption under the bar, e.g. "Loading world model". */
	static FString GetStatus();
	/** The map being loaded, without path or extension. Empty when it is not a Source map being loaded. */
	static FString GetMapName();
};
