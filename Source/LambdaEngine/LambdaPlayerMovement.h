#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PawnMovementComponent.h"
#include "LambdaPlayerMovement.generated.h"

class UCapsuleComponent;

/**
 * The player's movement, as CGameMovement does it: the move loop ported, not adapted. CategorizePosition to find
 * the ground, TryPlayerMove to slide along what it hits, StepMove to get up a step, ClipVelocity to take the
 * velocity out of a plane, and the Quake friction and acceleration. Unreal's part is reduced to sweeping a shape
 * through the world and saying what it hit.
 *
 * The shape it sweeps is a capsule, where Source sweeps a box, and the reason is the collision backend rather
 * than taste. Source traces its box against brush planes, which is exact. Our world is a triangle mesh, and a
 * swept box catches the seams between triangles - edge normals that belong to no face - which came out as the
 * player sticking to perfectly flat walls. A capsule rolls over the seams. It keeps the box's numbers: radius
 * 16 units, 72 tall standing, 36 ducked, feet at the bottom.
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
	void SetHullComponent(UCapsuleComponent* InHull) { Hull = InHull; }

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
	/**
	 * How far between standing and ducked the *view* is: 0 standing, 1 ducked.
	 *
	 * SetDuckedEyeOffset: Source moves the view across the duck as it happens, while the hull changes in one
	 * step at the end of it. Reading the view off bDucked instead makes the eye sit still for the whole
	 * transition and then move, which is felt as the crouch not responding.
	 */
	float GetDuckViewFraction() const { return DuckViewFraction; }
	bool IsDucking() const { return bDucking; }

	/** Where the feet are: the bottom of the box. */
	FVector GetFeetLocation() const;
	/** How tall the box is right now, in centimetres. */
	float GetHullHeight() const;

	/**
	 * Bumped every time the duck state changes, with whether that change happened in the air.
	 *
	 * The view needs to know: a duck begun off the ground lifts the feet by the whole hull difference in one
	 * frame, and only an equally sudden drop in the eye leaves the head still. A duck begun on the ground moves
	 * no feet, so the eye eases instead.
	 */
	int32 DuckChangeCount = 0;
	bool bLastDuckChangeAirborne = false;

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
	/** The hull's radius and half height, standing or ducked, in centimetres. */
	void HullDimensions(bool bDuckedHull, float& OutRadius, float& OutHalfHeight) const;
	/** The standing and ducked half heights differ by this much; the crouch-jump lift is exactly it. */
	float HullHalfHeightDelta() const;
	void SetGroundActor(AActor* NewGround, const FHitResult& Hit);
	/**
	 * PM_NudgePosition: if the hull has ended up inside something, walk it back out by trying small offsets
	 * until a spot fits - up first, then an expanding ring.
	 *
	 * GoldSrc unsticks this way, and it is the reliable way here too: asking the physics engine how far out of a
	 * *triangle mesh* a shape is (its MTD query) gives answers that are wrong as often as right, but "does the
	 * hull fit at this spot" is a question it always answers correctly. Without this, one frame that begins
	 * inside geometry is permanent - TryPlayerMove refuses start-solid sweeps, so the player never moves again.
	 */
	void NudgePosition();
	/** PM_TestPlayerPosition: does the hull, slightly shrunk, fit here? Shrunk so flush wall contact is not "stuck". */
	bool TestPlayerPosition(const FVector& Centre) const;

	UPROPERTY(Transient)
	TObjectPtr<UCapsuleComponent> Hull;

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
	float DuckTime = 0.0f;
	float DuckViewFraction = 0.0f;		// m_flDucktime, in seconds spent in the transition
};
