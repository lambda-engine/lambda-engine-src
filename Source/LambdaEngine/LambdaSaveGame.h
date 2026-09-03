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
 *   * the map, the player's position and angles, health, armour, suit, weapons, ammo and which weapon is out.
 *
 * What is NOT in it, and what a save therefore forgets: doors and buttons and their states, NPCs alive or
 * dead, physics props, and anything an entity was in the middle of doing. Loading restores you to a map at a
 * spot with your kit; it does not restore the world around you. That is a real limitation and the reason this
 * is a foundation rather than a finished feature - entity state needs each entity to describe its own fields,
 * which is what Source's datadesc is for and what would come next.
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

	/** Applied by the character when it spawns, if a Load armed one. Clears itself. */
	static void ApplyPendingRestore(ALambdaCharacter* Player);
	static bool HasPendingRestore();

	/** Where saves live: <moddir>/SAVE. */
	static FString SaveDirectory();

private:
	static FString SavePath(const FString& Name);
};
