#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SourceBSPFile.h"
#include "SourceBrushEntity.generated.h"

class UProceduralMeshComponent;
class ULambdaMaterialLibrary;
class FSourceBSPFile;

/**
 * Base for entities whose geometry is a BSP brush model ("model" "*N"), e.g. func_door_rotating, func_brush, func_wall.
 *
 * vbsp stores a brush model's vertices relative to the entity's "origin" keyvalue, so the mesh is built in that local
 * space and the actor is placed at the origin. Rotating the actor therefore pivots about the entity origin, which is
 * what Source does for rotating brush entities.
 */
UCLASS()
class LAMBDASOURCE_API ASourceBrushEntity : public AActor
{
	GENERATED_BODY()

public:
	ASourceBrushEntity();

	/** Builds the brush geometry and places the actor. Called right after spawning, before BeginPlay. */
	virtual void InitializeFromEntity(const FSourceBSPFile& Map, int32 ModelIndex, const FSourceEntity& InEntity,
		ULambdaMaterialLibrary* MaterialLibrary);

	/** Player pressed +USE while looking at this entity. */
	virtual void OnUsed(AActor* Activator) {}
	/** Whether +USE should consider this entity at all (CBaseEntity::ObjectCaps / FCAP_IMPULSE_USE). */
	virtual bool IsUsable() const { return false; }

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lambda")
	TObjectPtr<UProceduralMeshComponent> BrushMesh;

	/** The entity's keyvalues, as parsed from the BSP entity lump. */
	const FSourceEntity& GetEntity() const { return Entity; }
	int32 GetSpawnFlags() const { return SpawnFlags; }
	bool HasSpawnFlags(int32 Flags) const { return (SpawnFlags & Flags) != 0; }

	/** Entity origin in Source units (the pivot the mesh is built around). */
	const FVector3f& GetSourceOrigin() const { return SourceOrigin; }
	/** Current entity angles in Source (pitch, yaw, roll) degrees. */
	const FVector3f& GetSourceAngles() const { return SourceAngles; }
	/** Sets the actor rotation from Source angles, keeping SourceAngles authoritative. */
	void SetSourceAngles(const FVector3f& InAngles);

protected:
	FSourceEntity Entity;
	FVector3f SourceOrigin = FVector3f::ZeroVector;
	FVector3f SourceAngles = FVector3f::ZeroVector;
	int32 SpawnFlags = 0;
	int32 BrushModelIndex = INDEX_NONE;

	/** Local-space bounds of the brush mesh in UE units (used by the door's open-away-from-player logic). */
	FBox LocalBounds = FBox(ForceInit);
};
