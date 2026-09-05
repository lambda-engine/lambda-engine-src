#pragma once

#include "CoreMinimal.h"
#include "World/SourceEntity.h"
#include "SourceEnvParticles.generated.h"

class ASourceParticleSystem;
class ULambdaMaterialLibrary;

/**
 * env_particles: a particle system placed in the map, and info_particle_system, which is the same entity under
 * the name Source gives it (game/server/particle_system.cpp - CParticleSystem).
 *
 * Keyvalues: effect_name (the definition), start_active, angles (control point 0's orientation), and cpoint1..63
 * naming entities whose positions become the other control points - read in order and stopping at the first one
 * left blank, as Source does. Inputs: Start, Stop, DestroyImmediately.
 */
UCLASS()
class LAMBDASOURCE_API ASourceEnvParticles : public ASourceEntity
{
	GENERATED_BODY()

public:
	ASourceEnvParticles();

	virtual void InitializeEntity(const FSourceEntity& InEntity, ASourceBSPWorldActor* InWorldActor) override;
	virtual bool AcceptInput(const FString& InputName, AActor* Activator, AActor* Caller, const FString& Parameter) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	void SetMaterialLibrary(ULambdaMaterialLibrary* InMaterials) { MaterialLibrary = InMaterials; }

	/** InputStart / InputStop / InputDestroyImmediately. */
	void StartEffect();
	void StopEffect(bool bDestroyImmediately);

	const FString& GetEffectName() const { return EffectName; }
	bool IsEffectActive() const;

private:
	/** The cpointN targets, looked up once every entity in the map exists. */
	void ResolveControlPoints();

	UPROPERTY(Transient)
	TObjectPtr<ASourceParticleSystem> System;

	UPROPERTY(Transient)
	TObjectPtr<ULambdaMaterialLibrary> MaterialLibrary;

	FString EffectName;
	FVector3f Angles = FVector3f::ZeroVector;
	bool bStartActive = false;
	bool bControlPointsResolved = false;
	/** Target names for control points 1..63 (index 0 unused). */
	TArray<FString> ControlPointTargets;
	/** The resolved ones, followed each tick so a parented control point moves with its entity. */
	TArray<TWeakObjectPtr<AActor>> ControlPointActors;
};
