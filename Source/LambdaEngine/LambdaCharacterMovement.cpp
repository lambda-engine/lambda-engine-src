#include "LambdaCharacterMovement.h"

#include "Core/LambdaSourceSettings.h"

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
static float SvAirSpeedCap = 30.0f;
static FAutoConsoleVariableRef CVarSvAirSpeedCap(TEXT("sv_airspeedcap"), SvAirSpeedCap,
	TEXT("How much of the wish speed air acceleration will chase, in units."));

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
	bMaintainHorizontalGroundVelocity = true;
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
