#pragma once

#include "CoreMinimal.h"
#include "SourceGeometryBuilder.h"	// FSourceMeshSection

/**
 * Reader for Source studio models: the .mdl (header, materials, body parts), the .vvd (vertex data) and the .vtx
 * (hardware-optimised index data). A model needs all three, and Source checks that their checksums agree.
 *
 * This builds the model's *reference pose* - the vertices as stored in the .vvd - which is what a static render
 * needs. Bones are read for their names but no skinning or animation is applied, so animated models show their
 * bind pose.
 */
class LAMBDASOURCE_API FSourceMDLFile
{
public:
	static constexpr int32 MDL_IDENT = 'TSDI';	// "IDST"
	static constexpr int32 VVD_IDENT = 'VSDI';	// "IDSV"

	/** Loads "models/weapons/v_pistol.mdl" through the virtual file system, plus its .vvd and best .vtx. */
	bool Load(const FString& RelativeModelPath, float Scale, FString* OutError = nullptr);

	bool IsLoaded() const { return bLoaded; }
	const FString& GetModelName() const { return ModelName; }
	int32 GetVersion() const { return Version; }
	int32 GetNumBones() const { return NumBones; }

	/** Mesh sections in UE units and UE space, one per material, ready for a procedural mesh component. */
	TArray<FSourceMeshSection> Sections;

	/** Total triangles across all sections. */
	int32 GetNumTriangles() const;

private:
	/** Picks the best available .vtx for a model path (Source tries dx90, then dx80, then software). */
	bool ReadVtx(const FString& BasePath, TArray<uint8>& OutData, FString& OutUsedPath) const;

	FString ModelName;
	int32 Version = 0;
	int32 NumBones = 0;
	bool bLoaded = false;
};
