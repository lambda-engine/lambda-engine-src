#pragma once

#include "CoreMinimal.h"

class ALambdaCharacter;

/** One save on disk, as the load dialog needs to describe it. */
struct FLambdaSaveInfo
{
	FString Name;			// "quick", "autosave", "save01" - the file's stem
	FString MapName;		// the map it was taken on
	FDateTime When;			// file timestamp, for sorting and for showing
	FString Comment;		// what the player sees: usually the map name and the time
};

/**
 * Saving and restoring a game.
 *
 * Source keeps saves as .sav files under the mod's SAVE directory, with quick.sav and autosave.sav by name and
 * a rotating history behind them (host_saverestore.cpp; save_history_count). The naming and the directory are
 * kept, because a player and a mod author both already know them.
 *
 * The format is NOT Source's. Source serialises every entity through its datadesc into a binary block, which
 * only means anything to a build with the same class layout; ours is Valve KeyValues text, like every other
 * file this engine reads. What is in it is the honest limit of the system today:
 *
 *   * the map, the player's position and angles, health, armour, suit, weapons, ammo and which weapon is out;
 *   * every entity the map spawned, by its place in the spawn order: where it is, which way it faces, and
 *     whatever its own behaviour chose to write down - a door open or locked, a button pressed, a relay
 *     switched off, a trigger disabled, an NPC's health and whether it is still alive.
 *
 * That last part is Source's datadesc, turned round for a DLL boundary: rather than the engine walking a
 * table of member offsets, each entity names its own saved fields through ISaveState. An entity that wants
 * to survive a save says so; one that does not is rebuilt from its keyvalues, which is the right answer for
 * anything the map fully describes.
 *
 * What is still NOT in it: anything spawned at runtime rather than by the map - ragdolls, grenades in
 * flight, decals - and physics props' momentum. Those are restored as the map describes them, not as they
 * were left.
 */
class LAMBDAENGINE_API FLambdaSaveGame
{
public:
	/** Writes a save. Name is the file stem; "quick" and "autosave" are the two Source reserves. */
	static bool Save(UWorld* World, const FString& Name);

	/**
	 * Loads a save: opens its map and arms the player state to be applied once the player exists.
	 *
	 * The restore cannot happen here - the level has not loaded and the pawn does not exist yet - so the state
	 * is parked and ALambdaCharacter picks it up when it spawns, the same shape as the .auto console commands.
	 */
	static bool Load(UWorld* World, const FString& Name);

	/** Every save on disk, newest first. */
	static TArray<FLambdaSaveInfo> List();

	/** True if there is anything to load at all - what the death screen asks before offering to restore. */
	static bool HasAnySave();
	/** The newest save, or an empty name when there is none. */
	static FString NewestSaveName();

	/**
	 * Applied once the map's entities exist: puts back what happened to the world.
	 *
	 * Called from the world actor at the end of LoadMap, when every entity has spawned and read its
	 * keyvalues but before anything has had a chance to run.
	 */
	static void ApplyPendingWorldRestore(class ASourceBSPWorldActor* WorldActor);

	/** Applied by the character when it spawns, if a Load armed one. Clears itself. */
	static void ApplyPendingRestore(ALambdaCharacter* Player);
	static bool HasPendingRestore();

	/** Where saves live: <moddir>/SAVE. */
	static FString SaveDirectory();

private:
	static FString SavePath(const FString& Name);
};
