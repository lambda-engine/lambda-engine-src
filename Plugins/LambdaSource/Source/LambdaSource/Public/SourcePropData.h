#pragma once

#include "CoreMinimal.h"
#include "SourceKeyValues.h"

/**
 * One prop's prop_data (props_shared.h's IBreakableWithPropData). A model says which entry it uses in its own
 * keyvalues - "prop_data { base "Wooden.Medium" }" - and the entry itself lives in scripts/propdata.txt, where it
 * may name a base of its own to build on.
 */
struct LAMBDASOURCE_API FSourcePropDataEntry
{
	/** How much damage the prop takes before it comes apart. -1 when nothing said, which means it cannot break. */
	float Health = -1.0f;

	/** Damage modifiers: stone shrugs off bullets, wood does not much care for clubs. */
	float DmgModBullet = 1.0f;
	float DmgModClub = 1.0f;
	float DmgModExplosive = 1.0f;

	/** The chunk list in propdata.txt's BreakableModels a prop with no pieces of its own breaks into. */
	FString BreakableModel;
	int32 BreakableCount = 0;
	int32 BreakableSkin = 0;

	/** A prop that goes off when it breaks (explosive_damage / explosive_radius). */
	float ExplosiveDamage = 0.0f;
	float ExplosiveRadius = 0.0f;

	bool bAllowStatic = false;

	bool IsBreakable() const { return Health > 0.0f; }
};

/**
 * CPropData: scripts/propdata.txt, the file that says how much punishment each kind of prop takes and what it
 * leaves behind. Loaded once per level, as the game does at LevelInitPreEntity.
 *
 * Not ported: the interaction sections (physgun_interactions, fire_interactions, explosive_resist and the rest),
 * damage_table and the multiplayer break modes.
 */
class LAMBDASOURCE_API FSourcePropData
{
public:
	static FSourcePropData& Get();

	/** Reads scripts/propdata.txt. Safe to call again; a level reload re-reads the file. */
	void Load();

	/** ParsePropFromKV: the entry named by a model's own "prop_data" keyvalues, with its bases folded in. */
	bool ResolveForModel(const FString& ModelKeyValueText, FSourcePropDataEntry& OutEntry) const;

	/** The named entry from propdata.txt ("Wooden.Medium"), with its bases folded in. */
	bool Resolve(const FString& EntryName, FSourcePropDataEntry& OutEntry) const;

	/**
	 * GetRandomChunkModel: one of the models in a BreakableModels list ("WoodChunks"). The lists are sorted
	 * smallest first, and MaxSize limits how far up the list a prop of a given size may reach.
	 */
	FString GetRandomChunkModel(const FString& ChunkType, int32 MaxSize) const;

	bool IsLoaded() const { return bLoaded; }

private:
	/** Folds one propdata.txt section into an entry, recursing into its "base" first. */
	void ApplySection(const FSourceKeyValues& Section, FSourcePropDataEntry& Entry, int32 Depth) const;

	TMap<FString, FSourceKeyValues> Entries;
	TMap<FString, TArray<FString>> BreakableChunks;
	bool bLoaded = false;
};
