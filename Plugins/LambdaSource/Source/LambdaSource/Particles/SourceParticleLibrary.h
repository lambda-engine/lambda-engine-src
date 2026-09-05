#pragma once

#include "CoreMinimal.h"
#include "Particles/SourceParticleDefinition.h"

/**
 * Every particle system definition the game knows, by name (CParticleSystemMgr's definition list).
 *
 * Source reads the files named in particles/particles_manifest.txt, and each file's definitions replace any
 * earlier ones of the same name. That is done here too, and then every other particles/*.pcf reachable
 * through the mounts is read as well, adding only names not yet known - so a content pack (plugins/<x>/) can
 * bring its effects along without the mod's manifest having to list them, the same way it brings its
 * materials and sounds.
 *
 * Definitions only reachable as another one's child are registered too (ParseChildren does the same), so a
 * child can be spawned on its own by name.
 */
class LAMBDASOURCE_API FSourceParticleLibrary
{
public:
	static FSourceParticleLibrary& Get();

	/** Reads the manifest and every .pcf. Safe to call repeatedly; only the first call does work. */
	void Initialize();
	/** Drops everything so the next Initialize() re-reads. */
	void Reset();

	/**
	 * Reads one .pcf by relative path. bReplace = the file's definitions replace existing ones of the same name
	 * (manifest semantics); otherwise only new names are added. Returns the number of definitions read, or
	 * INDEX_NONE when the file could not be read or parsed.
	 */
	int32 LoadFile(const FString& RelativePath, bool bReplace);

	/** A definition by name (case-insensitive), or null. Initialises on first use. */
	TSharedPtr<const FSourceParticleDefinition> Find(const FString& Name);

	/** Every definition name, sorted, for listing. */
	void GetDefinitionNames(TArray<FString>& OutNames) const;
	int32 Num() const { return Definitions.Num(); }

private:
	FSourceParticleLibrary() = default;

	TSharedPtr<FSourceParticleDefinition> ParseDefinition(const TSharedPtr<FSourceDMXFile>& File, int32 ElementIndex,
		TMap<int32, TSharedPtr<FSourceParticleDefinition>>& ByElement, const FString& SourceFile);
	static void ParseOperators(const FSourceDMXFile& File, const FSourceDMXElement& Element, const TCHAR* ArrayName,
		TArray<FSourceParticleOperatorDef>& Out);

	TMap<FString, TSharedPtr<FSourceParticleDefinition>> Definitions;	// keyed by lower-case name
	TSet<FString> LoadedFiles;
	bool bInitialized = false;
};
