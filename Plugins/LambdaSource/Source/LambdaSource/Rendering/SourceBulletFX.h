#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SourceBulletFX.generated.h"

class ULambdaMaterialLibrary;
class UProceduralMeshComponent;
class UPointLightComponent;

/**
 * The short-lived things a shot leaves in the world: the tracer streak and, for anything that is not the
 * player's own view model, the muzzle flash and its light.
 *
 * Source draws these as temp entities (CTempEnts::MuzzleFlash / TracerSound), which is exactly what this is -
 * an actor that exists for a couple of frames and removes itself. The player's first-person flash is a
 * separate, tighter effect drawn on the view model with the "_noz" materials; this is the one everyone else
 * sees, and the one an NPC needs so the player can tell who is shooting at him.
 */
UCLASS()
class LAMBDASOURCE_API ASourceBulletFX : public AActor
{
	GENERATED_BODY()

public:
	ASourceBulletFX();

	/**
	 * A tracer from the muzzle to where the round landed. Source draws roughly one round in four as a tracer
	 * rather than all of them, which is what keeps a burst readable instead of a solid line of light.
	 */
	static void Tracer(UWorld* World, const FVector& Start, const FVector& End, ULambdaMaterialLibrary* Materials);

	/** A muzzle flash and its light at a world position, pointing down Direction. */
	static void MuzzleFlash(UWorld* World, const FVector& Position, const FVector& Direction,
		ULambdaMaterialLibrary* Materials);

	virtual void Tick(float DeltaSeconds) override;

private:
	void BuildTracer(const FVector& Start, const FVector& End, ULambdaMaterialLibrary* Materials);
	void BuildFlash(const FVector& Position, const FVector& Direction, ULambdaMaterialLibrary* Materials);

	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> Mesh;

	UPROPERTY(Transient)
	TObjectPtr<UPointLightComponent> Light;

	float DieTime = 0.0f;
};
