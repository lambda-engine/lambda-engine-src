#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SourceParticleEffect.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;
class ULambdaMaterialLibrary;

/** One SimpleParticle (particles_simple.h): a camera-facing sprite with a lifetime, a velocity and a colour ramp. */
struct FSourceSimpleParticle
{
	FVector Position = FVector::ZeroVector;		// cm
	FVector Velocity = FVector::ZeroVector;		// cm/s
	float Lifetime = 0.0f;						// m_flLifetime, seconds lived
	float DieTime = 0.5f;						// m_flDieTime
	float StartSize = 1.0f;						// m_uchStartSize, cm
	float EndSize = 1.0f;						// m_uchEndSize
	FLinearColor Color = FLinearColor::White;	// m_uchColor
	float StartAlpha = 1.0f;					// m_uchStartAlpha
	float EndAlpha = 0.0f;						// m_uchEndAlpha
	float Roll = 0.0f;							// m_flRoll, radians
	float RollDelta = 0.0f;						// m_flRollDelta, radians/s
	int32 MaterialIndex = 0;
};

/**
 * A CSimpleEmitter: a short-lived burst of sprite particles drawn as camera-facing quads, one mesh section per
 * material, under a shared gravity. Source's client effects (blood, dust, smoke puffs) are built from these, and
 * porting them means porting their particle parameters, not a particle system - so this keeps exactly the
 * SimpleParticle fields and nothing more. The actor removes itself once the last particle dies.
 */
UCLASS()
class LAMBDASOURCE_API ASourceParticleEffect : public AActor
{
	GENERATED_BODY()

public:
	ASourceParticleEffect();

	/** CSimpleEmitter::Create at a world position. */
	static ASourceParticleEffect* Create(UWorld* World, const FVector& Origin, ULambdaMaterialLibrary* Materials);

	/** SetGravity, in cm/s^2 (Source passes units/s^2; convert before calling). */
	void SetGravity(float GravityCmPerSec2) { Gravity = GravityCmPerSec2; }

	/** GetPMaterial: registers a Source sprite material ("effects/blood_core") and returns its section index. */
	int32 AddMaterial(const FString& SourceMaterialName);

	/** AddParticle: Position is world space. */
	void AddParticle(const FSourceSimpleParticle& Particle);

	virtual void Tick(float DeltaSeconds) override;

private:
	void RebuildMesh();

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UProceduralMeshComponent> Mesh;

	UPROPERTY(Transient)
	TObjectPtr<ULambdaMaterialLibrary> MaterialLibrary;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> Materials;

	TArray<FSourceSimpleParticle> Particles;
	float Gravity = 0.0f;
	bool bEverHadParticles = false;
};
