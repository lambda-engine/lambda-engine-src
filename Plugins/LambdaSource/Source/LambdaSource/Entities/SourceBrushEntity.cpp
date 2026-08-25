#include "Entities/SourceBrushEntity.h"
#include "Core/LambdaSourceModule.h"
#include "Core/LambdaSourceSettings.h"
#include "Core/SourceCoordinates.h"
#include "World/SourceGeometryBuilder.h"
#include "World/SourceBSPWorldActor.h"
#include "Engine/CollisionProfile.h"
#include "ProceduralMeshComponent.h"

ASourceBrushEntity::ASourceBrushEntity()
{
	PrimaryActorTick.bCanEverTick = false;

	BrushMesh = CreateDefaultSubobject<USourceBrushMeshComponent>(TEXT("BrushMesh"));
	RootComponent = BrushMesh;
	// Brush entities move (doors, platforms), so they cannot be Static.
	BrushMesh->SetMobility(EComponentMobility::Movable);
	BrushMesh->bUseComplexAsSimpleCollision = true;
	BrushMesh->bUseAsyncCooking = false;
	BrushMesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	BrushMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	BrushMesh->SetCastShadow(true);
}

void ASourceBrushEntity::InitializeFromEntity(const FSourceBSPFile& Map, int32 ModelIndex, const FSourceEntity& InEntity,
	ULambdaMaterialLibrary* MaterialLibrary, ASourceBSPWorldActor* InWorldActor)
{
	InitializeEntity(InEntity, InWorldActor);
	BrushModelIndex = ModelIndex;

	SourceOrigin = FVector3f::ZeroVector;
	Entity.GetVector(TEXT("origin"), SourceOrigin);
	SourceAngles = FVector3f::ZeroVector;
	Entity.GetVector(TEXT("angles"), SourceAngles);

	const float Scale = ULambdaSourceSettings::Get().UnitScale;

	// Geometry is stored relative to the entity origin, so build it as-is and place the actor at that origin: the
	// mesh is then pivoted exactly where Source rotates the brush model about.
	TArray<FSourceMeshSection> Sections;
	FSourceGeometryStats GeoStats;
	SourceGeometry::BuildModel(Map, ModelIndex, Scale, Sections, GeoStats);
	SourceGeometry::ApplyToComponent(BrushMesh, Sections, MaterialLibrary);

	LocalBounds = FBox(ForceInit);
	for (const FSourceMeshSection& Section : Sections)
	{
		for (const FVector& V : Section.Vertices)
		{
			LocalBounds += V;
		}
	}

	SetActorLocation(FSourceCoords::ToUE(SourceOrigin, Scale));
	SetSourceAngles(SourceAngles);

	UE_LOG(LogLambdaSource, Log, TEXT("%s (*%d) at Source(%s): %d faces, %d tris, %d sections, spawnflags %d"),
		*Entity.ClassName, ModelIndex, *SourceOrigin.ToString(), GeoStats.NumFaces, GeoStats.NumTriangles,
		Sections.Num(), SpawnFlags);
}

void ASourceBrushEntity::SetSourceOrigin(const FVector3f& InOrigin)
{
	SourceOrigin = InOrigin;
	SetActorLocation(FSourceCoords::ToUE(SourceOrigin, ULambdaSourceSettings::Get().UnitScale));
}

void ASourceBrushEntity::SetSourceAngles(const FVector3f& InAngles)
{
	SourceAngles = InAngles;
	SetActorRotation(FSourceCoords::AnglesToUE(SourceAngles));
}
