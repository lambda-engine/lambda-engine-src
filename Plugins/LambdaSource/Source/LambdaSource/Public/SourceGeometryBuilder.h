#pragma once

#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"

class FSourceBSPFile;
class ULambdaMaterialLibrary;

/** One material's worth of triangles from a BSP model, ready to hand to a UProceduralMeshComponent. */
struct LAMBDASOURCE_API FSourceMeshSection
{
	FString MaterialName;
	bool bVisible = true;
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UV0;
	TArray<FLinearColor> Colors;
	TArray<FProcMeshTangent> Tangents;
};

/** Face accounting for one BuildModel call. */
struct LAMBDASOURCE_API FSourceGeometryStats
{
	int32 NumFaces = 0;
	int32 NumRenderedFaces = 0;
	int32 NumCollisionOnlyFaces = 0;
	int32 NumSkippedFaces = 0;
	int32 NumDisplacementFaces = 0;
	int32 NumVertices = 0;
	int32 NumTriangles = 0;
	int32 NumNaturalWindingFaces = 0;
};

namespace SourceGeometry
{
	/**
	 * Converts one BSP model's faces into mesh sections grouped by material.
	 *
	 * Vertices are emitted in the model's own coordinate space, converted to UE units. For the world model (index 0)
	 * that space is world space; for brush-entity models (1..N) vbsp stores the geometry relative to the entity's
	 * "origin" keyvalue, so the result is already pivoted correctly for the entity to rotate around.
	 */
	LAMBDASOURCE_API void BuildModel(const FSourceBSPFile& Map, int32 ModelIndex, float Scale,
		TArray<FSourceMeshSection>& OutSections, FSourceGeometryStats& OutStats);

	/** Pushes sections into a procedural mesh component, assigning materials and hiding collision-only sections. */
	LAMBDASOURCE_API void ApplyToComponent(UProceduralMeshComponent* Mesh, TArray<FSourceMeshSection>& Sections,
		ULambdaMaterialLibrary* MaterialLibrary);
}
