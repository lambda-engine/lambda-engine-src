#pragma once

#include "CoreMinimal.h"

class ULambdaMaterialLibrary;
struct FHitResult;
enum class ESourceBloodColor : uint8;

namespace SourceImpact
{
	/** What a trace hit resolved to: the VMT it struck and the surface data behind it. */
	struct LAMBDASOURCE_API FSurfaceHitInfo
	{
		FString MaterialName;		// "dev/dev_measuregeneric01"
		FString SurfaceProp;		// $surfaceprop, e.g. "concrete"
		TCHAR GameMaterial = TEXT('C');
		FString BulletImpactSound;	// soundscript name, e.g. "Concrete.BulletImpact"
	};

	/**
	 * Resolves a trace hit to its surface. Source reads the texinfo at the impact point; because we group BSP
	 * faces by material into procedural mesh sections, the collision face index gives the same answer.
	 * Requires the trace to have been run with bReturnFaceIndex and complex collision.
	 */
	LAMBDASOURCE_API bool ResolveSurface(const FHitResult& Hit, ULambdaMaterialLibrary* Materials, FSurfaceHitInfo& OutInfo);

	/**
	 * UTIL_ImpactTrace / FX_Impact: stamps the decal the surface's game material calls for and plays its
	 * bullet impact sound. Does nothing for surfaces whose game material is '-' ("don't decal this surface").
	 * A hit on an NPC bleeds instead: SpawnBlood at the wound and TraceBleed onto the world behind it.
	 * ShotDirection is the bullet's travel direction; Damage sizes the bleed.
	 */
	LAMBDASOURCE_API void PlayImpact(const FHitResult& Hit, ULambdaMaterialLibrary* Materials, UObject* SoundOuter,
		const FVector& ShotDirection, float Damage);

	/**
	 * A bullet's trace. Source's bullets hit NPCs through their hitboxes (TraceToStudio), not their hulls: when the
	 * line hits an NPC with hitboxes it is tested against them - a miss passes through and the trace goes on.
	 * OutHitGroup is the hit group of the hitbox struck (HITGROUP_GENERIC for the world).
	 */
	LAMBDASOURCE_API bool TraceBullet(UWorld* World, const FVector& Start, const FVector& End, FCollisionQueryParams Params,
		FHitResult& OutHit, int32& OutHitGroup);

	/** Stamps one decal material at a hit (random roll, DecalSizeVariance), attached to what was hit. */
	LAMBDASOURCE_API void SpawnDecal(const FHitResult& Hit, ULambdaMaterialLibrary* Materials, const FString& DecalName);

	/** UTIL_BloodImpact -> FX_BloodBulletImpact: the blood spray at a wound. */
	LAMBDASOURCE_API void SpawnBlood(UWorld* World, ULambdaMaterialLibrary* Materials, const FVector& Origin,
		const FVector& Normal, ESourceBloodColor Color);

	/** CBaseEntity::TraceBleed: blood decals on the world behind the wound, along the shot. */
	LAMBDASOURCE_API void TraceBleed(UWorld* World, ULambdaMaterialLibrary* Materials, const FHitResult& Wound,
		const FVector& ShotDirection, float Damage, ESourceBloodColor Color, const TArray<const AActor*>& Ignore);

	/**
	 * CDecalEmitterSystem::LevelInitPreEntity + the surface property precache: builds every impact decal material
	 * (and its height tile) and decodes the bullet-impact sound of every surface the map's materials use, so the
	 * first shot does not pay for all of it in one frame.
	 */
	LAMBDASOURCE_API void Precache(ULambdaMaterialLibrary* Materials, UObject* SoundOuter);
}
