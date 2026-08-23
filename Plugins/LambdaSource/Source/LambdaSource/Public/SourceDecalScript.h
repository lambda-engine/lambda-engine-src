#pragma once

#include "CoreMinimal.h"

/** One weighted decal material inside a named group, e.g. "decals/concrete/shot3_subrect" weight 1. */
struct LAMBDASOURCE_API FSourceDecalEntry
{
	FString MaterialName;
	float Weight = 1.0f;
};

/**
 * A "Subrect" VMT: a decal that is one tile of a shared atlas material rather than a texture of its own.
 * Every HL2 bullet-impact decal is one of these.
 */
struct LAMBDASOURCE_API FSourceDecalSubrect
{
	/** The atlas material the tile is cut from, e.g. "decals/decals_mod2x". */
	FString SheetMaterial;
	/** Tile origin and size in texels within the atlas. */
	FVector2D Pos = FVector2D::ZeroVector;
	FVector2D Size = FVector2D(64.0, 64.0);
	/** Hammer units per texel: the decal's world size is Size * DecalScale. */
	float DecalScale = 0.1f;
	/** True when the atlas is a DecalModulate material (mod2x blending) rather than a translucent one. */
	bool bModulate = false;
};

/**
 * Port of CDecalEmitterSystem (game/shared/decals.cpp): scripts/decals_subrect.txt defines named decal groups and
 * a TranslationData section that maps a surface's game material character onto one of those groups.
 */
class LAMBDASOURCE_API FSourceDecalScript
{
public:
	static FSourceDecalScript& Get();

	void Initialize();
	void Reset();

	/**
	 * CDecalEmitterSystem::TranslateDecalForGameMaterial. Concrete passes straight through; anything else looks
	 * up the group registered for that game material, and '-' means "do not decal this surface".
	 */
	FString TranslateDecalForGameMaterial(const FString& DecalName, TCHAR GameMaterial) const;

	/** CDecalEmitterSystem::GetDecalIndexForName: a weighted random pick from a group. Empty if unknown. */
	FString PickDecalMaterial(const FString& GroupName) const;

	/** Resolves the game material straight to a decal material name, the whole GetImpactDecal chain in one call. */
	FString GetImpactDecalMaterial(TCHAR GameMaterial) const;

	int32 NumGroups() const { return Groups.Num(); }

private:
	TMap<FString, TArray<FSourceDecalEntry>> Groups;	// keyed by lower-case group name ("impact.concrete")
	TMap<TCHAR, FString> GameMaterialTranslation;		// 'M' -> "Impact.Metal"
	bool bInitialized = false;
};
