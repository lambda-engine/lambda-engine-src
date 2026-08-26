#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PawnMovementComponent.h"
#include "LambdaPlayerMovement.generated.h"

class UBoxComponent;

/**
 * The player's movement, as CGameMovement does it: a box, traced and slid by hand.
 *
 * Unreal's character is a capsule and cannot be anything else - UCharacterMovementComponent asks its owner for
 * GetCapsuleComponent() everywhere it works out a floor, a step or a penetration. Quake and Half-Life move an
 * axis-aligned box: SOLID_BBOX, (-16,-16,0) to (16,16,72) standing and (16,16,36) ducked. A box and a pill behave
 * differently in the places that matter - a flat bottom catches a ledge lip that a rounded one slides off, and a
 * square footprint reaches further into its diagonals than a circle of the same width - so the shape is not a
 * detail that can be approximated away.
 *
 * So this is the move loop itself, ported rather than adapted: CategorizePosition to find the ground,
 * TryPlayerMove to slide along what it hits, StepMove to get up a step, ClipVelocity to take the velocity out of
 * a plane, and the Quake friction and acceleration that were already here. Unreal's part is reduced to sweeping
 * a shape through the world and telling us what it hit.
 */
UCLASS()
class LAMBDAENGINE_API ULambdaPlayerMovement : public UPawnMovementComponent
{
	GENERATED_BODY()

public:
	ULambdaPlayerMovement();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual float GetMaxSpeed() const override { return MaxSpeedCm; }

	/** The box being moved. Set by the pawn that owns this. */
	void SetHullComponent(UBoxComponent* InHull) { Hull = InHull; }

	// ---- what the rest of the game asks about the player ----
	bool IsMovingOnGround() const { return bOnGround; }
	bool IsFalling() const { return !bOnGround; }
	AActor* GetGroundActor() const { return GroundActor.Get(); }
	/** The floor Unreal's character exposed as CurrentFloor.HitResult; kept so the same code can ask. */
	const FHitResult& GetFloorHit() const { return FloorHit; }

	/** IN_JUMP for this frame. */
	void Jump() { bJumpPressed = true; }
	/** IN_DUCK, held. */
	void SetWantsToDuck(bool bWants) { bWantsToDuck = bWants; }
	bool IsDucked() const { return bDucked; }
	bool IsDucking() const { return bDucking; }

	/** Where the feet are: the bottom of the box. */
	FVector GetFeetLocation() const;
	/** How tall the box is right now, in centimetres. */
	float GetHullHeight() const;

	/** m_surfaceFriction. */
	float SurfaceFriction = 1.0f;

	/** sv_maxspeed for this frame, in centimetres - the character sets it when sprinting or ducking. */
	float MaxSpeedCm = 0.0f;

	/** Fires when the player lands, so the game can react the way ACharacter::Landed let it. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnLanded, const FHitResult&);
	FOnLanded OnLanded;

protected:
	// ---- CGameMovement ----
	/** Works out whether we are standing on something, and on what. */
	void CategorizePosition();
	/** The trace-and-slide loop. Returns Source's blocked mask: 1 floor, 2 wall. */
	int32 TryPlayerMove(const FVector* FirstDest, const FHitResult* FirstTrace);
	/** Tries the move again lifted by a step, and keeps whichever got further. */
	void StepMove(const FVector& Destination, FHitResult& Trace);
	/** Takes the velocity out of a plane. */
	static int32 ClipVelocity(const FVector& In, const FVector& Normal, FVector& Out, float Overbounce);

	void FullWalkMove(float DeltaTime);
	void WalkMove(float DeltaTime);
	void AirMove(float DeltaTime);
	void ApplyFriction(float DeltaTime);
	void Accelerate(const FVector& WishDir, float WishSpeed, float Accel, float DeltaTime);
	void AirAccelerate(const FVector& WishDir, float WishSpeed, float Accel, float DeltaTime);
	bool CheckJumpButton(float DeltaTime);
	void Duck(float DeltaTime);
	void FinishDuck();
	void FinishUnDuck();
	bool CanUnDuckHere() const;

	void StartGravity(float DeltaTime);
	void FinishGravity(float DeltaTime);

	/** TracePlayerBBox: sweeps the player's box between two points. */
	bool TraceHull(const FVector& Start, const FVector& End, FHitResult& OutHit) const;
	/** The box's half extent right now, standing or ducked. */
	FVector HullHalfExtent(bool bDuckedHull) const;
	void SetGroundActor(AActor* NewGround, const FHitResult& Hit);

	UPROPERTY(Transient)
	TObjectPtr<UBoxComponent> Hull;

	FVector WishDirection = FVector::ZeroVector;
	float WishSpeed = 0.0f;

	bool bOnGround = false;
	TWeakObjectPtr<AActor> GroundActor;
	FHitResult FloorHit;

	bool bJumpPressed = false;
	bool bJumpHeldLastFrame = false;

	bool bWantsToDuck = false;
	bool bDucked = false;		// m_bDucked: the ducked hull is in effect
	bool bDucking = false;		// m_bDucking: the transition is running
	float DuckTime = 0.0f;		// m_flDucktime, in seconds spent in the transition
};
