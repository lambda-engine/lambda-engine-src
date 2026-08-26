#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SourceInfoLadder.generated.h"

struct FSourceEntity;

/**
 * info_ladder: a climbable volume.
 *
 * A mapper draws a func_ladder brush; vbsp eats the brush and writes this point entity in its place, carrying
 * the volume as mins/maxs keyvalues - which is why there is no model and nothing to render or collide with. The
 * player's movement asks two things of it: does my hull touch the volume, and which way does the ladder face.
 *
 * The facing is not authored anywhere, so it is inferred the way Counter-Strike's ladders infer it: the volume
 * is a thin slab, its thin horizontal axis is the way through it, and the normal is whichever end of that axis
 * points at whoever is asking.
 */
UCLASS()
class LAMBDASOURCE_API ASourceInfoLadder : public AActor
{
	GENERATED_BODY()

public:
	ASourceInfoLadder();

	void InitializeFromEntity(const FSourceEntity& Entity);

	/** The climbable volume, in world centimetres. */
	const FBox& GetVolume() const { return Volume; }

	/** The ladder's normal, facing the given point: along the slab's thin axis, toward the asker. */
	FVector GetNormalToward(const FVector& WorldPoint) const;

private:
	FBox Volume = FBox(ForceInit);
	/** The slab's thin horizontal axis: (1,0,0) or (0,1,0). */
	FVector ThinAxis = FVector(1.0f, 0.0f, 0.0f);
};
