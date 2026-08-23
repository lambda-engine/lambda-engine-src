#pragma once

#include "CoreMinimal.h"

class ULambdaMaterialLibrary;
struct FHitResult;

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
	 */
	LAMBDASOURCE_API void PlayImpact(const FHitResult& Hit, ULambdaMaterialLibrary* Materials, UObject* SoundOuter);

	/**
	 * CDecalEmitterSystem::LevelInitPreEntity + the surface property precache: builds every impact decal material
	 * (and its height tile) and decodes the bullet-impact sound of every surface the map's materials use, so the
	 * first shot does not pay for all of it in one frame.
	 */
	LAMBDASOURCE_API void Precache(ULambdaMaterialLibrary* Materials, UObject* SoundOuter);
}
