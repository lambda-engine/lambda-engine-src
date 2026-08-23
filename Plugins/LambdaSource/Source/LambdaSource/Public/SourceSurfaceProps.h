#pragma once

#include "CoreMinimal.h"

/** One entry from scripts/surfaceproperties*.txt (physprops' surfacedata_t, minus the physics fields we do not use). */
struct LAMBDASOURCE_API FSourceSurfaceProp
{
	FString Name;
	/** The CHAR_TEX_* character from decals.h that selects impact effects: 'C' concrete, 'M' metal, 'W' wood, ... */
	TCHAR GameMaterial = TEXT('C');

	FString BulletImpactSound;		// "bulletimpact"
	FString ScrapeRoughSound;		// "scraperough"
	FString ImpactHardSound;		// "impacthard"
	FString ImpactSoftSound;		// "impactsoft"
	/** "audiohardnessfactor": how hard this surface sounds when something else lands on it. */
	float AudioHardnessFactor = 1.0f;
	FString StepLeftSound;			// "stepleft"
	FString StepRightSound;			// "stepright"

	float Density = 2000.0f;
	float Elasticity = 0.25f;
	float Friction = 0.8f;
};

/**
 * Loads Source's surface property scripts, the table that turns a material's $surfaceprop into the sounds and
 * impact effects the surface produces. Driven by scripts/surfaceproperties_manifest.txt exactly as
 * physprops does, so a mod adding a file to the manifest is picked up with no code change.
 */
class LAMBDASOURCE_API FSourceSurfaceProps
{
public:
	static FSourceSurfaceProps& Get();

	/** Parses the manifest and every file it lists. Safe to call repeatedly; only the first call does work. */
	void Initialize();
	void Reset();

	/** Looks a surface property up by name ("concrete", "metal"). Case-insensitive. Null if unknown. */
	const FSourceSurfaceProp* Find(const FString& Name) const;

	/**
	 * The game material character for a material's $surfaceprop, or 'C' (concrete) when the material declares
	 * none - which is what GetImpactDecal falls back to in fx_impact.cpp.
	 */
	TCHAR GetGameMaterial(const FString& SurfacePropName) const;

	int32 Num() const { return Props.Num(); }

private:
	void LoadFile(const FString& RelativePath);

	TMap<FString, FSourceSurfaceProp> Props;	// keyed by lower-case name
	bool bInitialized = false;
};
