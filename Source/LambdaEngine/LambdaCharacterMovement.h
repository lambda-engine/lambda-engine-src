#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "LambdaCharacterMovement.generated.h"

/**
 * Quake's player movement, which is the movement Source inherited and then tightened.
 *
 * Unreal accelerates towards a target velocity and brakes towards zero, which gives movement that stops when you
 * stop asking for it. Quake does something different and stranger: friction bleeds speed off every frame, and
 * acceleration only adds along the direction you are asking for, and only up to a wish speed measured *along that
 * direction*. Turn while airborne and the wish direction turns with you, so the dot product against your existing
 * velocity stays small, so the game keeps handing you speed - which is where air strafing and bunny hopping come
 * from. They are not features anybody wrote; they fall out of these three functions.
 *
 * This is CGameMovement::Friction, ::Accelerate and ::AirAccelerate (game/shared/gamemovement.cpp), which are
 * Quake's PM_Friction, PM_Accelerate and PM_AirAccelerate with the names changed. Half-Life 2 differs mainly in
 * what it does *around* them - it clamps speed after a jump to stop bunny hopping, and that is deliberately not
 * ported here.
 *
 * The cvars are Source's, and their values are Source's defaults. Speeds are in Hammer units, converted on use.
 */
UCLASS()
class LAMBDAENGINE_API ULambdaCharacterMovement : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	ULambdaCharacterMovement();

	/**
	 * Unreal calls this from both PhysWalking and PhysFalling to work out the frame's velocity; replacing it is
	 * what swaps the movement model out while leaving Unreal's collision, stepping and floor handling alone.
	 */
	virtual void CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration) override;

	/**
	 * CGameMovement::CheckJumpButton: a ducked player may jump.
	 *
	 * Unreal bans it twice over - once on the character (CanJumpInternal) and once here, where CanAttemptJump
	 * refuses while bWantsToCrouch. Source refuses only while the unduck transition is running, and has a branch
	 * specifically for jumping while ducked.
	 */
	virtual bool CanAttemptJump() const override;

	/**
	 * CGameMovement::Duck's timing, which is physical and not cosmetic.
	 *
	 * Unreal shrinks the capsule the frame the key goes down. Source starts a TIME_TO_DUCK clock and only
	 * finishes the duck when it runs out - unless the player leaves the ground first, in which case it finishes
	 * that instant, and finishing in the air is what tucks the feet up by the whole hull difference. That is why
	 * crouch-then-jump in quick succession is a crouch jump in Half-Life: the jump interrupts the transition and
	 * the tuck happens airborne. With an instant crouch there is nothing left to tuck by the time the jump goes
	 * off, and the order only works one way round.
	 */
	virtual void UpdateCharacterStateBeforeMovement(float DeltaSeconds) override;

	/**
	 * How far between standing and ducked the view is: 0 standing, 1 ducked. SetDuckedEyeOffset - the view
	 * crosses the transition while the hull changes in one step at the end of it.
	 */
	float GetDuckViewFraction() const { return DuckViewFraction; }
	/** Jumping off a ladder: let go, shoved off its face at 270 units (CGameMovement::LadderMove's IN_JUMP). */
	virtual bool DoJump(bool bReplayingMoves, float DeltaTime) override;

	/** m_surfaceFriction: how slippery what we are standing on is. Ice would lower it; nothing does yet. */
	float SurfaceFriction = 1.0f;

	/** MOVE_Custom submodes. */
	static constexpr uint8 CMOVE_Ladder = 1;

	bool IsOnLadder() const { return MovementMode == MOVE_Custom && CustomMovementMode == CMOVE_Ladder; }

protected:
	/** The climbing itself: MOVE_Custom / CMOVE_Ladder. LadderMove's attach test lives in
	 *  UpdateCharacterStateBeforeMovement, beside the duck timing - both run before each frame's physics. */
	virtual void PhysCustom(float DeltaTime, int32 Iterations) override;

	/** The ladder volume the hull touches (expanded a little), or null. */
	class ASourceInfoLadder* FindTouchedLadder(const FVector& Probe) const;

	float DuckTimer = 0.0f;			// m_flDucktime, roughly: seconds spent in the duck transition
	float DuckViewFraction = 0.0f;

	/** m_vecLadderNormal: out of the ladder's face, toward the climber. */
	FVector LadderNormal = FVector::ZeroVector;
	/**
	 * No re-grabbing until this time. Jumping off shoves the player 270 units away, but a frame is short and
	 * the reach is two units, so with forward still held the next frame would take hold again before the shove
	 * has moved anybody anywhere - the jump looked like nothing at all.
	 */
	float LadderRegrabTime = 0.0f;
	/** PM_Friction: bleed speed off, with a floor under how much so that stopping is not asymptotic. */
	void ApplyGroundFriction(float DeltaTime);
	/** PM_Accelerate. */
	void QuakeAccelerate(const FVector& WishDir, float WishSpeed, float Accel, float DeltaTime);
	/** PM_AirAccelerate: the same, but the wish speed is capped first. That cap is the whole trick. */
	void QuakeAirAccelerate(const FVector& WishDir, float WishSpeed, float Accel, float DeltaTime);
};
