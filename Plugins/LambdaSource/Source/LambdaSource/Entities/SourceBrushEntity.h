#pragma once

#include "CoreMinimal.h"
#include "World/SourceEntity.h"
#include "Formats/SourceBSPFile.h"
#include "World/SourceGeometryBuilder.h"
#include "SourceBrushEntity.generated.h"

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
class LAMBDASOURCE_API ASourceBrushEntity : public ASourceEntity
{
	GENERATED_BODY()

public:
	ASourceBrushEntity();

	/** Builds the brush geometry and places the actor. Called right after spawning, before BeginPlay. */
	virtual void InitializeFromEntity(const FSourceBSPFile& Map, int32 ModelIndex, const FSourceEntity& InEntity,
		ULambdaMaterialLibrary* MaterialLibrary, ASourceBSPWorldActor* InWorldActor);

	/** Player pressed +USE while looking at this entity. */
	virtual void OnUsed(AActor* Activator) {}
	/** Whether +USE should consider this entity at all (CBaseEntity::ObjectCaps / FCAP_IMPULSE_USE). */
	virtual bool IsUsable() const { return false; }

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lambda")
	TObjectPtr<USourceBrushMeshComponent> BrushMesh;

	/** Entity origin in Source units (the pivot the mesh is built around). */
	const FVector3f& GetSourceOrigin() const { return SourceOrigin; }
	/** Current entity angles in Source (pitch, yaw, roll) degrees. */
	const FVector3f& GetSourceAngles() const { return SourceAngles; }
	/** Sets the actor rotation from Source angles, keeping SourceAngles authoritative. */
	void SetSourceAngles(const FVector3f& InAngles);

protected:
	FVector3f SourceOrigin = FVector3f::ZeroVector;
	FVector3f SourceAngles = FVector3f::ZeroVector;
	int32 BrushModelIndex = INDEX_NONE;

	/** Sets the actor location from a Source-space position, keeping SourceOrigin authoritative. */
	void SetSourceOrigin(const FVector3f& InOrigin);

	/** Local-space bounds of the brush mesh in UE units (used by the door's open-away-from-player logic). */
	FBox LocalBounds = FBox(ForceInit);
};
