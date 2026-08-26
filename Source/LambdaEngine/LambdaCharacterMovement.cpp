#include "LambdaCharacterMovement.h"

#include "Core/LambdaSourceSettings.h"
#include "Entities/SourceInfoLadder.h"

#include "Components/CapsuleComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"

// movevars_shared.cpp, with Source's defaults. Held as floats so they can be changed while the game runs.
static float SvFriction = 4.0f;
static FAutoConsoleVariableRef CVarSvFriction(TEXT("sv_friction"), SvFriction, TEXT("World friction."));

static float SvStopSpeed = 100.0f;
static FAutoConsoleVariableRef CVarSvStopSpeed(TEXT("sv_stopspeed"), SvStopSpeed,
	TEXT("Minimum stopping speed when on ground, in units."));

static float SvAccelerate = 10.0f;
static FAutoConsoleVariableRef CVarSvAccelerate(TEXT("sv_accelerate"), SvAccelerate, TEXT("Ground acceleration."));

static float SvAirAccelerate = 10.0f;
static FAutoConsoleVariableRef CVarSvAirAccelerate(TEXT("sv_airaccelerate"), SvAirAccelerate,
	TEXT("Air acceleration. This is what air strafing is made of; raise it for more control, zero it for none."));

// CGameMovement::GetAirSpeedCap. Thirty units is the number every Quake-derived game uses, and the reason air
// strafing works at all: see QuakeAirAccelerate.
static float SvMaxVelocity = 3500.0f;
static FAutoConsoleVariableRef CVarSvMaxVelocity(TEXT("sv_maxvelocity"), SvMaxVelocity,
	TEXT("Maximum speed any ballistically moving object is allowed to attain per axis, in units."));

static float SvAirSpeedCap = 30.0f;
static FAutoConsoleVariableRef CVarSvAirSpeedCap(TEXT("sv_airspeedcap"), SvAirSpeedCap,
	TEXT("How much of the wish speed air acceleration will chase, in units."));

namespace
{
	// gamemovement.h
	constexpr float TIME_TO_DUCK = 0.4f;
	constexpr float TIME_TO_UNDUCK = 0.2f;
}

ULambdaCharacterMovement::ULambdaCharacterMovement()
{
	// Full air control, which sounds like the opposite of what a Quake port wants and is not.
	//
	// PhysFalling does not hand CalcVelocity the player's actual input: it works out
	// GetFallingLateralAcceleration - the input scaled by AirControl - and substitutes that for Acceleration
	// while CalcVelocity runs. Setting AirControl to zero, on the reasoning that our own air acceleration
	// replaces Unreal's, therefore fed the wish direction nothing at all, and there was no air movement of any
	// kind: jump and hold forward and you went straight up and came straight back down.
	//
	// So the input is passed through whole and QuakeAirAccelerate does the limiting, which is where the limit
	// belongs - sv_airspeedcap is what says how much of it counts.
	AirControl = 1.0f;
	// And passed through unchanged: Unreal boosts air control below a speed threshold, which would quietly
	// scale the wish direction we read back out of it.
	AirControlBoostMultiplier = 1.0f;
	AirControlBoostVelocityThreshold = 0.0f;

	// Unreal will not let a crouched character walk off a ledge. Source has no such rule - duck at the edge of a
	// drop in Half-Life and you go over it - so the rule goes.
	bCanWalkOffLedgesWhenCrouching = true;
	bMaintainHorizontalGroundVelocity = true;
}

void ULambdaCharacterMovement::UpdateCharacterStateBeforeMovement(float DeltaSeconds)
{
	ACharacter* Owner = CharacterOwner.Get();
	const bool bCrouchedNow = Owner && Owner->bIsCrouched;

	if (bWantsToCrouch && !bCrouchedNow && IsMovingOnGround())
	{
		// The transition. While it runs the hull stays standing and only the view moves - and the clock, not the
		// key, decides when the hull changes. SimpleSpline's easing, as SetDuckedEyeOffset applies it.
		DuckTimer += DeltaSeconds;
		if (DuckTimer < TIME_TO_DUCK)
		{
			DuckViewFraction = FMath::SmoothStep(0.0f, 1.0f, DuckTimer / TIME_TO_DUCK);
			bWantsToCrouch = false;		// not yet: keep Unreal's instant crouch from running
			Super::UpdateCharacterStateBeforeMovement(DeltaSeconds);
			bWantsToCrouch = true;
			// No ladder check this frame: grabbing on mid-transition would climb with a half-ducked view anyway.
			return;
		}
	}

	if (bWantsToCrouch)
	{
		// Finishing: the clock ran out on the ground, or the ground went away - "finish ducking immediately if
		// duck time is over or not on ground" - and an airborne finish tucks the feet (the OnStartCrouch lift).
		// The view goes to ducked in the same step; with the feet coming up, an eased eye would throw the head.
		DuckViewFraction = 1.0f;
	}
	else
	{
		DuckTimer = 0.0f;
		if (IsFalling())
		{
			// Standing up in the air drops the feet in one step, and the eye goes with them or the head lurches.
			DuckViewFraction = 0.0f;
		}
		else
		{
			// TIME_TO_UNDUCK: the view eases back up. The hull change stays instant on the ground, which is a
			// deliberate divergence - Source also refuses to jump during its unduck transition, and keeping the
			// stand instant is what lets crouch-jump-crouch flow without that refusal.
			DuckViewFraction = FMath::Max(0.0f, DuckViewFraction - DeltaSeconds / TIME_TO_UNDUCK);
		}
	}

	Super::UpdateCharacterStateBeforeMovement(DeltaSeconds);

	// ---- CGameMovement::LadderMove's attach test, on info_ladder volumes ----
	//
	// Already climbing, the wish direction is into the ladder; otherwise it is where the player is pushing, and
	// with no input there is no attaching. Two units of reach. A mapper draws func_ladder brushes and vbsp turns
	// each into an info_ladder point entity carrying the volume as mins/maxs - nothing solid, nothing rendered -
	// so this is a box test, the way Counter-Strike ladders work, not HL2's CONTENTS_LADDER trace.
	if (!HasValidData() || MovementMode == MOVE_None)
	{
		return;
	}

	// Just jumped off: the shove needs time to carry the player out of reach before a grab is offered again.
	if (!IsOnLadder() && GetWorld()->GetTimeSeconds() < LadderRegrabTime)
	{
		return;
	}

	// CGameMovement::LadderMove's attach test. Already climbing, look into the ladder; otherwise look where the
	// player is pushing, and no input means no attaching.
	FVector WishDir;
	if (IsOnLadder())
	{
		WishDir = -LadderNormal;
	}
	else
	{
		WishDir = Acceleration.GetSafeNormal();
		if (WishDir.IsNearlyZero())
		{
			return;
		}
	}

	// LadderDistance: two units along the wish is close enough to take hold.
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const FVector Probe = UpdatedComponent->GetComponentLocation() + WishDir * 2.0f * Scale;
	ASourceInfoLadder* Ladder = FindTouchedLadder(Probe);

	if (Ladder)
	{
		LadderNormal = Ladder->GetNormalToward(UpdatedComponent->GetComponentLocation());
		if (!IsOnLadder())
		{
			SetMovementMode(MOVE_Custom, CMOVE_Ladder);
		}
	}
	else if (IsOnLadder())
	{
		// Climbed off the end of it.
		SetMovementMode(MOVE_Falling);
	}
}

bool ULambdaCharacterMovement::CanAttemptJump() const
{
	// Unreal's, less the "and not crouching" it adds - and a ladder counts, because jumping is how you let go.
	return IsJumpAllowed() && (IsMovingOnGround() || IsFalling() || IsOnLadder());
}

bool ULambdaCharacterMovement::DoJump(bool bReplayingMoves, float DeltaTime)
{
	if (IsOnLadder())
	{
		// LadderMove's IN_JUMP: off the face at 270 units, and gravity takes over.
		Velocity = LadderNormal * 270.0f * ULambdaSourceSettings::Get().UnitScale;
		LadderRegrabTime = GetWorld()->GetTimeSeconds() + 0.4f;
		SetMovementMode(MOVE_Falling);
		return true;
	}
	return Super::DoJump(bReplayingMoves, DeltaTime);
}

void ULambdaCharacterMovement::CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration)
{
	const bool bGround = IsMovingOnGround();
	if (!HasValidData() || DeltaTime < MIN_TICK_TIME || (!bGround && !IsFalling()))
	{
		// Swimming, flying, ladders: nothing here has an opinion about those, so Unreal keeps them.
		Super::CalcVelocity(DeltaTime, Friction, bFluid, BrakingDeceleration);
		return;
	}

	// Unreal has already turned this frame's input into Acceleration, clamped to MaxAcceleration. Reading the
	// wish direction and how hard it is being asked for back out of it is what lets the rest of the character -
	// input bindings, sprint, crouch - stay exactly as it was.
	FVector WishDir = Acceleration;
	WishDir.Z = 0.0f;
	const float InputScale = (MaxAcceleration > KINDA_SMALL_NUMBER)
		? FMath::Min(1.0f, WishDir.Size() / MaxAcceleration)
		: 0.0f;
	WishDir = WishDir.GetSafeNormal();
	const float WishSpeed = GetMaxSpeed() * InputScale;	// sv_maxspeed, in centimetres

	if (bGround)
	{
		ApplyGroundFriction(DeltaTime);
		QuakeAccelerate(WishDir, WishSpeed, SvAccelerate, DeltaTime);
	}
	else
	{
		// Horizontal only. Unreal applies gravity itself in PhysFalling, and Quake's air acceleration never
		// touched the vertical either - the wish direction it works from is flattened.
		QuakeAirAccelerate(WishDir, WishSpeed, SvAirAccelerate, DeltaTime);
	}

	// CheckVelocity: each axis is clamped to sv_maxvelocity. A surf ramp with nothing at the bottom of it will
	// otherwise hand out speed without limit.
	const float MaxVel = SvMaxVelocity * ULambdaSourceSettings::Get().UnitScale;
	Velocity.X = FMath::Clamp(Velocity.X, -MaxVel, MaxVel);
	Velocity.Y = FMath::Clamp(Velocity.Y, -MaxVel, MaxVel);
	Velocity.Z = FMath::Clamp(Velocity.Z, -MaxVel, MaxVel);
}

void ULambdaCharacterMovement::ApplyGroundFriction(float DeltaTime)
{
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const float Speed = Velocity.Size2D();
	if (Speed < 0.1f * Scale)
	{
		return;
	}

	// Below the stop speed, bleed as though we were doing the stop speed. Without this the last of the velocity
	// is bled proportionally and never quite reaches zero, so a player who let go of the key would drift.
	const float StopSpeed = SvStopSpeed * Scale;
	const float Control = (Speed < StopSpeed) ? StopSpeed : Speed;
	const float Drop = Control * SvFriction * SurfaceFriction * DeltaTime;

	const float NewSpeed = FMath::Max(0.0f, Speed - Drop);
	if (NewSpeed != Speed)
	{
		const float Proportion = NewSpeed / Speed;
		Velocity.X *= Proportion;
		Velocity.Y *= Proportion;
	}
}

void ULambdaCharacterMovement::QuakeAccelerate(const FVector& WishDir, float WishSpeed, float Accel, float DeltaTime)
{
	// How much of our speed is already going the way we want. Speed sideways to the wish direction is free -
	// it is not counted against the wish speed, which is why you can carry speed through a turn.
	const float CurrentSpeed = FVector::DotProduct(Velocity, WishDir);
	const float AddSpeed = WishSpeed - CurrentSpeed;
	if (AddSpeed <= 0.0f)
	{
		return;
	}
	const float AccelSpeed = FMath::Min(Accel * DeltaTime * WishSpeed * SurfaceFriction, AddSpeed);
	Velocity += AccelSpeed * WishDir;
}

void ULambdaCharacterMovement::QuakeAirAccelerate(const FVector& WishDir, float WishSpeed, float Accel, float DeltaTime)
{
	const float Scale = ULambdaSourceSettings::Get().UnitScale;

	// The cap applies to how much speed we are willing to chase, but NOT to how fast we chase it: addspeed is
	// measured against the capped wish speed while accelspeed is computed from the full one. That asymmetry is
	// the whole of air strafing. Hold a strafe key, turn the mouse with it, and the wish direction stays nearly
	// perpendicular to where you are already going - so the dot product below stays near zero, addspeed stays
	// positive, and the game keeps adding speed you never asked it for.
	const float CappedWishSpeed = FMath::Min(WishSpeed, SvAirSpeedCap * Scale);

	const float CurrentSpeed = FVector::DotProduct(Velocity, WishDir);
	const float AddSpeed = CappedWishSpeed - CurrentSpeed;
	if (AddSpeed <= 0.0f)
	{
		return;
	}
	const float AccelSpeed = FMath::Min(Accel * WishSpeed * DeltaTime * SurfaceFriction, AddSpeed);
	Velocity += AccelSpeed * WishDir;
}

ASourceInfoLadder* ULambdaCharacterMovement::FindTouchedLadder(const FVector& Probe) const
{
	const UWorld* World = GetWorld();
	const ACharacter* Owner = CharacterOwner.Get();
	if (!World || !Owner)
	{
		return nullptr;
	}
	// The hull as a box around the capsule, moved to the probe position; a ladder is touched when the boxes meet.
	const UCapsuleComponent* Capsule = Owner->GetCapsuleComponent();
	const FVector Extent(Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleRadius(),
		Capsule->GetScaledCapsuleHalfHeight());
	const FBox HullBox(Probe - Extent, Probe + Extent);

	for (TActorIterator<ASourceInfoLadder> It(const_cast<UWorld*>(World)); It; ++It)
	{
		if (It->GetVolume().Intersect(HullBox))
		{
			return *It;
		}
	}
	return nullptr;
}


void ULambdaCharacterMovement::PhysCustom(float DeltaTime, int32 Iterations)
{
	if (CustomMovementMode != CMOVE_Ladder)
	{
		Super::PhysCustom(DeltaTime, Iterations);
		return;
	}
	ACharacter* Owner = CharacterOwner.Get();
	if (!Owner || DeltaTime < MIN_TICK_TIME)
	{
		return;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;

	// The rest is CGameMovement::LadderMove's climb, verbatim in UE vectors.
	//
	// The intended velocity is rebuilt in the *view* basis, pitch included: Source's forwardmove rides
	// m_vecForward, which points where the player looks, so climbing follows the eyes - look up and push
	// forward to go up, look down to go down. Acceleration cannot be used directly here because the input
	// bindings flattened it to the yaw plane.
	const FRotator View = Owner->GetControlRotation();
	const FVector ViewForward = View.Vector();
	const FVector ViewRight = FRotationMatrix(FRotator(0.0f, View.Yaw, 0.0f)).GetUnitAxis(EAxis::Y);
	const FRotator YawOnly(0.0f, View.Yaw, 0.0f);
	const FVector YawForward = YawOnly.Vector();

	const float ClimbSpeed = 200.0f * Scale;	// MAX_CLIMB_SPEED
	const float ForwardMove = FMath::Sign(FVector::DotProduct(Acceleration, YawForward))
		* (FMath::Abs(FVector::DotProduct(Acceleration.GetSafeNormal(), YawForward)) > 0.3f ? ClimbSpeed : 0.0f);
	const float RightMove = FMath::Sign(FVector::DotProduct(Acceleration, ViewRight))
		* (FMath::Abs(FVector::DotProduct(Acceleration.GetSafeNormal(), ViewRight)) > 0.3f ? ClimbSpeed : 0.0f);

	if (ForwardMove == 0.0f && RightMove == 0.0f)
	{
		// No input: hang where you are.
		Velocity = FVector::ZeroVector;
	}
	else
	{
		const FVector Intended = ViewForward * ForwardMove + ViewRight * RightMove;

		// Decompose against the ladder: perp lies across its face, TmpUp runs up it.
		const FVector Perp = FVector::CrossProduct(FVector::UpVector, LadderNormal).GetSafeNormal();
		const float NormalComp = FVector::DotProduct(Intended, LadderNormal);
		const FVector Cross = LadderNormal * NormalComp;
		const FVector Lateral = Intended - Cross;
		const FVector TmpUp = FVector::CrossProduct(LadderNormal, Perp);

		Velocity = Lateral - NormalComp * TmpUp;

		// On the floor and pushing away from the ladder: step off it rather than climb.
		if (IsMovingOnGround() && NormalComp > 0.0f)
		{
			Velocity += ClimbSpeed * LadderNormal;
			SetMovementMode(MOVE_Falling);
		}
	}

	// Move, sliding along whatever the hull meets - the wall behind the ladder, the platform lip at the top.
	const FVector Delta = Velocity * DeltaTime;
	FHitResult Hit;
	SafeMoveUpdatedComponent(Delta, UpdatedComponent->GetComponentQuat(), true, Hit);
	if (Hit.IsValidBlockingHit())
	{
		SlideAlongSurface(Delta, 1.0f - Hit.Time, Hit.Normal, Hit, true);
	}
}
