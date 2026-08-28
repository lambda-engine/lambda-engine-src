#include "World/SourceGeometryBuilder.h"
#include "Materials/LambdaMaterialLibrary.h"
#include "Core/LambdaSourceModule.h"
#include "Formats/SourceBSPFile.h"
#include "Core/SourceCoordinates.h"

void SourceGeometry::BuildModel(const FSourceBSPFile& Map, int32 ModelIndex, float Scale,
	TArray<FSourceMeshSection>& OutSections, FSourceGeometryStats& OutStats,
	ULambdaMaterialLibrary* MaterialLibrary)
{
	// One lookup per material rather than per face; a map is thousands of faces over a few dozen materials.
	TMap<int32, FIntPoint> MappingSizes;
	using namespace SourceBSP;

	OutSections.Reset();
	OutStats = FSourceGeometryStats();

	if (!Map.Models.IsValidIndex(ModelIndex))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("BuildModel: model %d does not exist (map has %d)"), ModelIndex, Map.Models.Num());
		return;
	}
	const dmodel_t& Model = Map.Models[ModelIndex];

	TMap<FString, int32> SectionByMaterial;
	int32 CollisionSectionIndex = INDEX_NONE;

	const int32 FirstFace = FMath::Max(0, Model.firstface);
	const int32 EndFace = FMath::Min(Map.Faces.Num(), Model.firstface + Model.numfaces);
	OutStats.NumFaces = FMath::Max(0, EndFace - FirstFace);

	for (int32 FaceIndex = FirstFace; FaceIndex < EndFace; ++FaceIndex)
	{
		const dface_t& Face = Map.Faces[FaceIndex];
		if (Face.dispinfo != -1)
		{
			++OutStats.NumDisplacementFaces; // TODO: displacement surfaces
			continue;
		}
		if (Face.numedges < 3)
		{
			++OutStats.NumSkippedFaces;
			continue;
		}

		const texinfo_t* TexInfo = Map.TexInfos.IsValidIndex(Face.texinfo) ? &Map.TexInfos[Face.texinfo] : nullptr;
		const int32 SurfFlags = TexInfo ? TexInfo->flags : 0;
		if (SurfFlags & (SURF_HINT | SURF_SKIP | SURF_TRIGGER))
		{
			++OutStats.NumSkippedFaces;
			continue;
		}
		const bool bCollisionOnly = (SurfFlags & (SURF_NODRAW | SURF_SKY | SURF_SKY2D)) != 0;

		// Pick/create the section for this face.
		int32 SectionIndex = INDEX_NONE;
		if (bCollisionOnly)
		{
			if (CollisionSectionIndex == INDEX_NONE)
			{
				CollisionSectionIndex = OutSections.AddDefaulted();
				OutSections[CollisionSectionIndex].bVisible = false;
				OutSections[CollisionSectionIndex].MaterialName = TEXT("<collision>");
			}
			SectionIndex = CollisionSectionIndex;
			++OutStats.NumCollisionOnlyFaces;
		}
		else
		{
			const FString MaterialName = TexInfo ? Map.GetTexDataName(TexInfo->texdata).ToLower() : FString();
			if (const int32* Existing = SectionByMaterial.Find(MaterialName))
			{
				SectionIndex = *Existing;
			}
			else
			{
				SectionIndex = OutSections.AddDefaulted();
				OutSections[SectionIndex].MaterialName = MaterialName;
				SectionByMaterial.Add(MaterialName, SectionIndex);
			}
			++OutStats.NumRenderedFaces;
		}
		FSourceMeshSection& Section = OutSections[SectionIndex];

		// Polygon vertices in Source space, following the surfedge direction.
		TArray<FVector3f, TInlineAllocator<32>> Poly;
		for (int32 e = 0; e < Face.numedges; ++e)
		{
			const int32 SurfEdge = Map.SurfEdges[Face.firstedge + e];
			const uint16 VertexIndex = (SurfEdge >= 0) ? Map.Edges[SurfEdge].v[0] : Map.Edges[-SurfEdge].v[1];
			Poly.Add(Map.Vertices[VertexIndex]);
		}

		// Face normal from the winding (Source convention): faces are wound so the surfedge order is clockwise seen
		// from the visible/front side in Source's right-handed space, so the visible normal = -Newell(polygon).
		FVector3f SrcNewell = FVector3f::ZeroVector;
		for (int32 i = 0; i < Poly.Num(); ++i)
		{
			SrcNewell += FVector3f::CrossProduct(Poly[i], Poly[(i + 1) % Poly.Num()]);
		}
		FVector3f SrcNormal = (-SrcNewell).GetSafeNormal();
		if (SrcNormal.IsNearlyZero())
		{
			SrcNormal = Map.Planes.IsValidIndex(Face.planenum) ? Map.Planes[Face.planenum].normal.ToVector3f().GetSafeNormal() : FVector3f::UpVector;
		}
		const FVector NormalUE = FSourceCoords::ToUEDirection(SrcNormal);

		// Texture mapping: u = (P . s + s_off) / width, v = (P . t + t_off) / height, all in Source space.
		FVector3f SAxis = FVector3f::ZeroVector;
		FVector3f TAxis = FVector3f::ZeroVector;
		float SOffset = 0.0f, TOffset = 0.0f, TexWidth = 1.0f, TexHeight = 1.0f;
		if (TexInfo)
		{
			SAxis = FVector3f(TexInfo->textureVecsTexelsPerWorldUnits[0][0], TexInfo->textureVecsTexelsPerWorldUnits[0][1], TexInfo->textureVecsTexelsPerWorldUnits[0][2]);
			SOffset = TexInfo->textureVecsTexelsPerWorldUnits[0][3];
			TAxis = FVector3f(TexInfo->textureVecsTexelsPerWorldUnits[1][0], TexInfo->textureVecsTexelsPerWorldUnits[1][1], TexInfo->textureVecsTexelsPerWorldUnits[1][2]);
			TOffset = TexInfo->textureVecsTexelsPerWorldUnits[1][3];
			if (Map.TexDatas.IsValidIndex(TexInfo->texdata))
			{
				TexWidth = (float)FMath::Max(1, Map.TexDatas[TexInfo->texdata].width);
				TexHeight = (float)FMath::Max(1, Map.TexDatas[TexInfo->texdata].height);
			}
		}
		FVector TangentUE = FSourceCoords::ToUEDirection(SAxis);
		if (TangentUE.IsNearlyZero())
		{
			TangentUE = FVector::ForwardVector;
		}

		const int32 BaseIndex = Section.Vertices.Num();
		for (const FVector3f& P : Poly)
		{
			Section.Vertices.Add(FSourceCoords::ToUE(P, Scale));
			Section.Normals.Add(NormalUE);
			const float U = (FVector3f::DotProduct(P, SAxis) + SOffset) / TexWidth;
			const float V = (FVector3f::DotProduct(P, TAxis) + TOffset) / TexHeight;
			Section.UV0.Add(FVector2D(U, V));
			Section.Tangents.Add(FProcMeshTangent(TangentUE, false));
			Section.Colors.Add(FLinearColor::White);
		}

		// Choose the fan orientation whose UE front-face normal (-Newell in UE space) matches the visible normal.
		FVector Newell = FVector::ZeroVector;
		for (int32 i = 0; i < Poly.Num(); ++i)
		{
			const FVector& A = Section.Vertices[BaseIndex + i];
			const FVector& B = Section.Vertices[BaseIndex + ((i + 1) % Poly.Num())];
			Newell += FVector::CrossProduct(A, B);
		}
		const double NaturalDot = FVector::DotProduct(-Newell, NormalUE);
		const bool bReverse = !(NaturalDot > KINDA_SMALL_NUMBER);
		if (!bReverse)
		{
			++OutStats.NumNaturalWindingFaces;
		}
		for (int32 i = 1; i + 1 < Poly.Num(); ++i)
		{
			Section.Triangles.Add(BaseIndex);
			if (bReverse)
			{
				Section.Triangles.Add(BaseIndex + i + 1);
				Section.Triangles.Add(BaseIndex + i);
			}
			else
			{
				Section.Triangles.Add(BaseIndex + i);
				Section.Triangles.Add(BaseIndex + i + 1);
			}
		}
		OutStats.NumTriangles += Poly.Num() - 2;
	}

	for (const FSourceMeshSection& Section : OutSections)
	{
		OutStats.NumVertices += Section.Vertices.Num();
	}
}

void SourceGeometry::ApplyToComponent(UProceduralMeshComponent* Mesh, TArray<FSourceMeshSection>& Sections,
	ULambdaMaterialLibrary* MaterialLibrary, bool bCreateCollision)
{
	if (!Mesh)
	{
		return;
	}
	Mesh->ClearAllMeshSections();

	USourceBrushMeshComponent* BrushMesh = Cast<USourceBrushMeshComponent>(Mesh);
	if (BrushMesh)
	{
		BrushMesh->SectionMaterialNames.Reset(Sections.Num());
	}

	for (int32 SectionIndex = 0; SectionIndex < Sections.Num(); ++SectionIndex)
	{
		FSourceMeshSection& Section = Sections[SectionIndex];
		Mesh->CreateMeshSection_LinearColor(SectionIndex, Section.Vertices, Section.Triangles, Section.Normals,
			Section.UV0, Section.Colors, Section.Tangents, bCreateCollision);
		if (!Section.bVisible)
		{
			Mesh->SetMeshSectionVisible(SectionIndex, false);
		}
		else if (MaterialLibrary)
		{
			Mesh->SetMaterial(SectionIndex, MaterialLibrary->GetMaterial(Section.MaterialName));
		}
		if (BrushMesh)
		{
			BrushMesh->SectionMaterialNames.Add(Section.MaterialName);
		}
		UE_LOG(LogLambdaSource, Verbose, TEXT("  section %d: %s verts=%d tris=%d%s"), SectionIndex, *Section.MaterialName,
			Section.Vertices.Num(), Section.Triangles.Num() / 3, Section.bVisible ? TEXT("") : TEXT(" (collision only)"));
	}
}

USourceBrushMeshComponent::USourceBrushMeshComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Map geometry opts into lighting channel 1 as well as the default, which is what lets the muzzle flash
	// light the room while skipping the view model (which stays on channel 0 only).
	LightingChannels.bChannel0 = true;
	LightingChannels.bChannel1 = true;
}

FString USourceBrushMeshComponent::GetMaterialNameForFaceIndex(int32 FaceIndex) const
{
	int32 SectionIndex = INDEX_NONE;
	// UProceduralMeshComponent maps a cooked-collision face back to the section that produced it.
	GetMaterialFromCollisionFaceIndex(FaceIndex, SectionIndex);
	return SectionMaterialNames.IsValidIndex(SectionIndex) ? SectionMaterialNames[SectionIndex] : FString();
}
