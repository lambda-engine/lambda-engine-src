#pragma once

#include "CoreMinimal.h"

class FSourceMDLFile;
class ULambdaMaterialLibrary;
class USkeletalMesh;

/**
 * Builds a USkeletalMesh from a loaded Source studio model, so that the GPU skins it.
 *
 * Source stores exactly what a skinned mesh needs - a bone hierarchy with a bind pose, and vertices carrying up
 * to three bone influences each - and studiomdl's skinning is the same equation the hardware runs: a vertex is
 * moved by its bones' matrices, each of which takes a reference-pose vertex into posed model space. Handing that
 * to the renderer instead of doing it on the CPU every frame is the difference between an HL:A model costing a
 * chunk of the frame and costing nothing.
 *
 * The mesh is shared: it holds no pose, so every zombie in a map can draw from one, where CPU skinning needed a
 * private copy of the vertices per instance.
 */
class LAMBDASOURCE_API FSourceSkeletalMesh
{
public:
	/**
	 * Returns the mesh for this model's current bodygroup selection, building it the first time it is asked for.
	 * The cache is keyed on the model path and the bodygroups, because changing a bodygroup changes which meshes
	 * the model draws.
	 */
	static USkeletalMesh* GetOrBuild(const FString& ModelPath, const FSourceMDLFile& Model,
		ULambdaMaterialLibrary* Materials, const FString& BodygroupKey,
		const TSet<int32>* HiddenBones = nullptr);

	/** Drops every cached mesh (map change). */
	static void FlushCache();

private:
	static USkeletalMesh* Build(const FSourceMDLFile& Model, ULambdaMaterialLibrary* Materials, const FString& DebugName,
		const TSet<int32>* HiddenBones);
};
