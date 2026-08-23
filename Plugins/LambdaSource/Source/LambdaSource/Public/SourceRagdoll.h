#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SourceMDLFile.h"
#include "SourceRagdoll.generated.h"

class USourceStudioModelComponent;
class UProceduralMeshComponent;
class UPhysicsConstraintComponent;
class FSourcePHYFile;
enum class ESourceBloodColor : uint8;

/**
 * A ragdoll in the shape Source builds one (ragdoll_shared.cpp RagdollCreate): one rigid body per solid of the
 * model's .phy, placed on its bone in the pose the model died in, joined by the .phy's ragdoll constraints, kicked
 * by the killing blow's force. Every frame the bodies' transforms are read back as bone matrices and the studio
 * model is re-skinned from them, exactly as C_ClientRagdoll drives the bones from its vphysics objects; bones with
 * no solid ride along with their nearest simulated ancestor at their death-pose offset.
 *
 * Bodies are plain procedural-mesh components with convex collision (no mesh sections) so the whole thing stays a
 * runtime construct - no physics asset, no skeletal mesh.
 */
UCLASS()
class LAMBDASOURCE_API ASourceRagdoll : public AActor
{
	GENERATED_BODY()

public:
	ASourceRagdoll();

	/**
	 * Builds the ragdoll for a posed model. ForceImpulse is in kg*cm/s, applied at ForcePosition (world) to the
	 * nearest body and spread over the rest by mass (RagdollCreate); InitialVelocity is given to every body.
	 */
	static ASourceRagdoll* Create(UWorld* World, USourceStudioModelComponent* ModelComponent, const FSourcePHYFile& Phy,
		const FVector& ForceImpulse, const FVector& ForcePosition, const FVector& InitialVelocity,
		ESourceBloodColor BloodColor, float Lifetime);

	virtual void Tick(float DeltaSeconds) override;

	ESourceBloodColor GetBloodColor() const { return BloodColor; }
	int32 GetNumBodies() const { return Bodies.Num(); }

private:
	bool Build(USourceStudioModelComponent* ModelComponent, const FSourcePHYFile& Phy);
	void ApplyForces(const FVector& ForceImpulse, const FVector& ForcePosition, const FVector& InitialVelocity);
	void UpdatePose();

	UPROPERTY(Transient)
	TWeakObjectPtr<USourceStudioModelComponent> Model;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UProceduralMeshComponent>> Bodies;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UPhysicsConstraintComponent>> Joints;

	/** Bone each body drives. */
	TArray<int32> BodyBone;
	/** Per bone: the body driving it, or INDEX_NONE. */
	TArray<int32> BoneBody;
	/** The pose at death, per bone, in the model's space (UE). */
	TArray<FTransform> DeathPose;
	/** The model component's world transform at death; bodies are read back relative to it. */
	FTransform ModelToWorld = FTransform::Identity;

	ESourceBloodColor BloodColor;
	float TotalMass = 0.0f;
};
