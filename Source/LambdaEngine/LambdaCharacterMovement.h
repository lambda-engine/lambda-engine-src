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

	/** m_surfaceFriction: how slippery what we are standing on is. Ice would lower it; nothing does yet. */
	float SurfaceFriction = 1.0f;

protected:
	/** PM_Friction: bleed speed off, with a floor under how much so that stopping is not asymptotic. */
	void ApplyGroundFriction(float DeltaTime);
	/** PM_Accelerate. */
	void QuakeAccelerate(const FVector& WishDir, float WishSpeed, float Accel, float DeltaTime);
	/** PM_AirAccelerate: the same, but the wish speed is capped first. That cap is the whole trick. */
	void QuakeAirAccelerate(const FVector& WishDir, float WishSpeed, float Accel, float DeltaTime);
};
