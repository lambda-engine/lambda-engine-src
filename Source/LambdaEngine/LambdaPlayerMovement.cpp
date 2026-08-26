#include "LambdaPlayerMovement.h"

#include "Core/LambdaSourceSettings.h"

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"

// movevars_shared.cpp, with Source's defaults.
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

static float SvAirSpeedCap = 30.0f;
static FAutoConsoleVariableRef CVarSvAirSpeedCap(TEXT("sv_airspeedcap"), SvAirSpeedCap,
	TEXT("How much of the wish speed air acceleration will chase, in units."));

namespace
{
	// gamemovement.cpp
	constexpr float DIST_EPSILON = 0.03125f;		// in Hammer units
	constexpr int32 MAX_CLIP_PLANES = 5;
	constexpr float NON_JUMP_VELOCITY = 140.0f;		// units/s: above this we are going up, not standing
	/** A surface is a floor when its normal points this far up (CGameMovement's 0.7 everywhere). */
	constexpr float WalkableNormalZ = 0.7f;
	constexpr float TIME_TO_DUCK = 0.4f;
	constexpr float TIME_TO_UNDUCK = 0.2f;
}

ULambdaPlayerMovement::ULambdaPlayerMovement()
{
	PrimaryComponentTick.bCanEverTick = true;
}

float ULambdaPlayerMovement::GetHullHeight() const
{
	return Hull ? Hull->GetScaledCapsuleHalfHeight() * 2.0f : 0.0f;
}

FVector ULambdaPlayerMovement::GetFeetLocation() const
{
	if (!UpdatedComponent)
	{
		return FVector::ZeroVector;
	}
	return UpdatedComponent->GetComponentLocation() - FVector(0.0f, 0.0f, GetHullHeight() * 0.5f);
}

void ULambdaPlayerMovement::HullDimensions(bool bDuckedHull, float& OutRadius, float& OutHalfHeight) const
{
	// VEC_HULL_MIN/MAX's numbers on a capsule: 32 wide and 72 tall standing, 36 tall ducked.
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	OutRadius = 16.0f * Scale;
	OutHalfHeight = (bDuckedHull ? 18.0f : 36.0f) * Scale;
}

float ULambdaPlayerMovement::HullHalfHeightDelta() const
{
	// Half of (72 - 36) units: the standing half height less the ducked one.
	return 18.0f * ULambdaSourceSettings::Get().UnitScale;
}

bool ULambdaPlayerMovement::TraceHull(const FVector& Start, const FVector& End, FHitResult& OutHit) const
{
	const UWorld* World = GetWorld();
	if (!World || !Hull)
	{
		return false;
	}
	// TracePlayerBBox, with a capsule where Source has a box: swept at its true size, and the physics engine's
	// own contact offset keeps it a hair clear of what it lands on.
	FCollisionQueryParams Params(SCENE_QUERY_STAT(PlayerHull), /*bTraceComplex=*/ false, PawnOwner);
	Params.bReturnFaceIndex = true;
	return World->SweepSingleByChannel(OutHit, Start, End, FQuat::Identity, ECC_Pawn,
		FCollisionShape::MakeCapsule(Hull->GetScaledCapsuleRadius(), Hull->GetScaledCapsuleHalfHeight()), Params);
}

int32 ULambdaPlayerMovement::ClipVelocity(const FVector& In, const FVector& Normal, FVector& Out, float Overbounce)
{
	// CGameMovement::ClipVelocity
	int32 Blocked = 0x00;
	if (Normal.Z > 0.0f)
	{
		Blocked |= 0x01;	// floor
	}
	if (Normal.Z == 0.0f)
	{
		Blocked |= 0x02;	// vertical: a wall or a step
	}

	const float Backoff = FVector::DotProduct(In, Normal) * Overbounce;
	Out = In - Normal * Backoff;

	// Iterate once against numerical drift back into the plane.
	const float Adjust = FVector::DotProduct(Out, Normal);
	if (Adjust < 0.0f)
	{
		Out -= Normal * Adjust;
	}
	return Blocked;
}

void ULambdaPlayerMovement::SetGroundActor(AActor* NewGround, const FHitResult& Hit)
{
	const bool bWasFalling = !bOnGround;
	GroundActor = NewGround;
	bOnGround = NewGround != nullptr;
	if (bOnGround)
	{
		FloorHit = Hit;
		if (bWasFalling)
		{
			OnLanded.Broadcast(Hit);
		}
	}
}

bool ULambdaPlayerMovement::TestPlayerPosition(const FVector& Centre) const
{
	const UWorld* World = GetWorld();
	if (!World || !Hull)
	{
		return true;
	}
	// Shrunk by half a unit so resting flush against a wall does not read as being stuck inside it.
	const float Shrink = 0.5f * ULambdaSourceSettings::Get().UnitScale;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(PlayerFits), false, PawnOwner);
	return !World->OverlapBlockingTestByChannel(Centre, FQuat::Identity, ECC_Pawn,
		FCollisionShape::MakeCapsule(
			FMath::Max(1.0f, Hull->GetScaledCapsuleRadius() - Shrink),
			FMath::Max(1.0f, Hull->GetScaledCapsuleHalfHeight() - Shrink)),
		Params);
}

void ULambdaPlayerMovement::NudgePosition()
{
	// PM_NudgePosition. Not stuck is the common case, and costs one overlap test a frame.
	if (!UpdatedComponent || TestPlayerPosition(UpdatedComponent->GetComponentLocation()))
	{
		return;
	}

	// Stuck. Try small offsets until somewhere fits: straight up first, since the usual way in is through the
	// floor, then an expanding ring. Asking the engine how far out of a triangle mesh we are (its MTD query)
	// gives answers that are wrong as often as right; "does the hull fit here" it always answers correctly.
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const FVector Base = UpdatedComponent->GetComponentLocation();

	static const float Rises[] = { 0.125f, 0.5f, 1.0f, 2.0f, 4.0f };
	for (float Rise : Rises)
	{
		const FVector Spot = Base + FVector(0.0f, 0.0f, Rise * Scale);
		if (TestPlayerPosition(Spot))
		{
			UpdatedComponent->SetWorldLocation(Spot, false, nullptr, ETeleportType::TeleportPhysics);
			return;
		}
	}
	static const float Rings[] = { 1.0f, 2.0f, 4.0f };
	for (float Ring : Rings)
	{
		for (int32 X = -1; X <= 1; ++X)
		{
			for (int32 Y = -1; Y <= 1; ++Y)
			{
				for (int32 Z = 0; Z <= 1; ++Z)
				{
					if (X == 0 && Y == 0 && Z == 0)
					{
						continue;
					}
					const FVector Spot = Base + FVector(X * Ring, Y * Ring, Z * Ring) * Scale;
					if (TestPlayerPosition(Spot))
					{
						UpdatedComponent->SetWorldLocation(Spot, false, nullptr, ETeleportType::TeleportPhysics);
						return;
					}
				}
			}
		}
	}
	// Nowhere nearby fits: stay put rather than teleport somewhere surprising. GoldSrc gives up here too.
}

void ULambdaPlayerMovement::CategorizePosition()
{
	// CGameMovement::CategorizePosition
	SurfaceFriction = 1.0f;
	if (!UpdatedComponent)
	{
		return;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;

	// Going up fast enough means we jumped, and are not standing on anything.
	if (Velocity.Z > NON_JUMP_VELOCITY * Scale)
	{
		SetGroundActor(nullptr, FHitResult());
		return;
	}

	// Two units down is enough to find the floor we are resting on.
	const FVector Start = UpdatedComponent->GetComponentLocation();
	const FVector End = Start - FVector(0.0f, 0.0f, 2.0f * Scale);

	FHitResult Hit;
	const bool bHit = TraceHull(Start, End, Hit);
	if (!bHit || Hit.ImpactNormal.Z < WalkableNormalZ)
	{
		SetGroundActor(nullptr, FHitResult());
		// Sliding down something too steep to stand on: less grip, so friction bites less.
		if (Velocity.Z > 0.0f)
		{
			SurfaceFriction = 0.25f;
		}
		return;
	}

	SetGroundActor(Hit.GetActor() ? Hit.GetActor() : GetOwner(), Hit);
	// Settle a hair above where the sweep stopped, never flush. Trusting the engine's own contact offset here
	// was tried and does not hold against the world's triangle mesh: a settle that lands flush makes the next
	// sweep - a jump's included - begin solid, and TryPlayerMove throws the velocity away.
	if (Hit.bBlockingHit)
	{
		UpdatedComponent->SetWorldLocation(Hit.Location + FVector(0.0f, 0.0f, DIST_EPSILON * Scale),
			false, nullptr, ETeleportType::TeleportPhysics);
	}
}

int32 ULambdaPlayerMovement::TryPlayerMove(const FVector* FirstDest, const FHitResult* FirstTrace)
{
	// CGameMovement::TryPlayerMove: move, and where something is in the way, slide along it and try again with
	// what is left of the frame. Four bumps, because a corner can need two planes and a crease a third.
	const int32 NumBumps = 4;
	int32 Blocked = 0;
	int32 NumPlanes = 0;
	FVector Planes[MAX_CLIP_PLANES];

	FVector OriginalVelocity = Velocity;
	const FVector PrimalVelocity = Velocity;
	FVector NewVelocity = FVector::ZeroVector;

	float TimeLeft = GetWorld()->GetDeltaSeconds();
	float AllFraction = 0.0f;

	for (int32 Bump = 0; Bump < NumBumps; ++Bump)
	{
		if (Velocity.IsNearlyZero())
		{
			break;
		}
		const FVector Start = UpdatedComponent->GetComponentLocation();
		const FVector End = Start + Velocity * TimeLeft;

		FHitResult Hit;
		bool bHit;
		if (FirstDest && FirstTrace && End.Equals(*FirstDest, 0.01f))
		{
			Hit = *FirstTrace;
			bHit = Hit.bBlockingHit;
		}
		else
		{
			bHit = TraceHull(Start, End, Hit);
		}

		AllFraction += bHit ? Hit.Time : 1.0f;

		if (Hit.bStartPenetrating)
		{
			// Stuck in something solid: give up rather than squirt out of it.
			Velocity = FVector::ZeroVector;
			return 4;
		}

		if (!bHit)
		{
			UpdatedComponent->SetWorldLocation(End, false, nullptr, ETeleportType::TeleportPhysics);
			break;	// moved the whole way
		}

		if (Hit.Time > 0.0f)
		{
			UpdatedComponent->SetWorldLocation(Hit.Location, false, nullptr, ETeleportType::TeleportPhysics);
			OriginalVelocity = Velocity;
			NumPlanes = 0;
		}

		if (Hit.ImpactNormal.Z > WalkableNormalZ)
		{
			Blocked |= 1;	// floor
		}
		if (Hit.ImpactNormal.Z == 0.0f)
		{
			Blocked |= 2;	// wall or step
		}

		TimeLeft -= TimeLeft * Hit.Time;

		if (NumPlanes >= MAX_CLIP_PLANES)
		{
			// Wedged into more planes than can be reasoned about.
			Velocity = FVector::ZeroVector;
			break;
		}
		Planes[NumPlanes++] = Hit.ImpactNormal;

		if (NumPlanes == 1 && !bOnGround)
		{
			// In the air against a single plane: slide along it, bouncing off anything that is not a floor.
			for (int32 i = 0; i < NumPlanes; ++i)
			{
				ClipVelocity(OriginalVelocity, Planes[i], NewVelocity, 1.0f);
			}
			Velocity = NewVelocity;
			OriginalVelocity = NewVelocity;
		}
		else
		{
			int32 i = 0;
			for (; i < NumPlanes; ++i)
			{
				ClipVelocity(OriginalVelocity, Planes[i], Velocity, 1.0f);

				int32 j = 0;
				for (; j < NumPlanes; ++j)
				{
					if (j != i && FVector::DotProduct(Velocity, Planes[j]) < 0.0f)
					{
						break;	// this clip pushes us into another plane
					}
				}
				if (j == NumPlanes)
				{
					break;		// clear of all of them
				}
			}

			if (i == NumPlanes)
			{
				// Not clear of any single plane, so go along the crease where two of them meet.
				if (NumPlanes != 2)
				{
					Velocity = FVector::ZeroVector;
					break;
				}
				const FVector Dir = FVector::CrossProduct(Planes[0], Planes[1]).GetSafeNormal();
				Velocity = Dir * FVector::DotProduct(Dir, Velocity);
			}

			// Turned back on ourselves: stop rather than run backwards along the wall.
			if (FVector::DotProduct(Velocity, PrimalVelocity) <= 0.0f)
			{
				Velocity = FVector::ZeroVector;
				break;
			}
		}
	}

	if (AllFraction == 0.0f)
	{
		Velocity = FVector::ZeroVector;
	}
	return Blocked;
}

void ULambdaPlayerMovement::StepMove(const FVector& Destination, FHitResult& Trace)
{
	// CGameMovement::StepMove: do the move; then do it again from a step higher and keep whichever got further.
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const float StepSize = ULambdaSourceSettings::Get().StepHeightUnits * Scale;

	const FVector StartPos = UpdatedComponent->GetComponentLocation();
	const FVector StartVel = Velocity;

	// First, the plain move.
	FVector EndPos = Destination;
	TryPlayerMove(&EndPos, &Trace);
	const FVector DownPos = UpdatedComponent->GetComponentLocation();
	const FVector DownVel = Velocity;

	// Then the same move, begun a step up.
	UpdatedComponent->SetWorldLocation(StartPos, false, nullptr, ETeleportType::TeleportPhysics);
	Velocity = StartVel;

	FHitResult Up;
	if (TraceHull(StartPos, StartPos + FVector(0.0f, 0.0f, StepSize + DIST_EPSILON * Scale), Up))
	{
		if (!Up.bStartPenetrating)
		{
			UpdatedComponent->SetWorldLocation(Up.Location, false, nullptr, ETeleportType::TeleportPhysics);
		}
	}
	else
	{
		UpdatedComponent->SetWorldLocation(StartPos + FVector(0.0f, 0.0f, StepSize + DIST_EPSILON * Scale),
			false, nullptr, ETeleportType::TeleportPhysics);
	}
	TryPlayerMove(nullptr, nullptr);

	// And back down onto whatever is under us there.
	const FVector AfterUp = UpdatedComponent->GetComponentLocation();
	FHitResult Down;
	const bool bDownHit = TraceHull(AfterUp, AfterUp - FVector(0.0f, 0.0f, StepSize + DIST_EPSILON * Scale), Down);
	if (!bDownHit || Down.ImpactNormal.Z < WalkableNormalZ)
	{
		// Nothing to stand on up there, so the stepped-up attempt is no good.
		UpdatedComponent->SetWorldLocation(DownPos, false, nullptr, ETeleportType::TeleportPhysics);
		Velocity = DownVel;
		return;
	}
	if (!Down.bStartPenetrating)
	{
		UpdatedComponent->SetWorldLocation(Down.Location, false, nullptr, ETeleportType::TeleportPhysics);
	}

	// Keep whichever attempt covered more ground.
	const FVector UpPos = UpdatedComponent->GetComponentLocation();
	const float DownDist = FVector::DistSquared2D(DownPos, StartPos);
	const float UpDist = FVector::DistSquared2D(UpPos, StartPos);
	if (DownDist > UpDist)
	{
		UpdatedComponent->SetWorldLocation(DownPos, false, nullptr, ETeleportType::TeleportPhysics);
		Velocity = DownVel;
	}
	else
	{
		// Stepping up does not add vertical speed; the box was simply placed higher.
		Velocity.Z = DownVel.Z;
	}
}

void ULambdaPlayerMovement::ApplyFriction(float DeltaTime)
{
	// PM_Friction
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const float Speed = Velocity.Size2D();
	if (Speed < 0.1f * Scale)
	{
		return;
	}
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

void ULambdaPlayerMovement::Accelerate(const FVector& WishDir, float InWishSpeed, float Accel, float DeltaTime)
{
	// PM_Accelerate: only speed along the wish direction counts, which is what lets speed survive a turn.
	const float CurrentSpeed = FVector::DotProduct(Velocity, WishDir);
	const float AddSpeed = InWishSpeed - CurrentSpeed;
	if (AddSpeed <= 0.0f)
	{
		return;
	}
	const float AccelSpeed = FMath::Min(Accel * DeltaTime * InWishSpeed * SurfaceFriction, AddSpeed);
	Velocity += AccelSpeed * WishDir;
}

void ULambdaPlayerMovement::AirAccelerate(const FVector& WishDir, float InWishSpeed, float Accel, float DeltaTime)
{
	// PM_AirAccelerate. How much speed is chased is capped; how fast it is chased is not, and that asymmetry is
	// the whole of air strafing.
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const float CappedWishSpeed = FMath::Min(InWishSpeed, SvAirSpeedCap * Scale);

	const float CurrentSpeed = FVector::DotProduct(Velocity, WishDir);
	const float AddSpeed = CappedWishSpeed - CurrentSpeed;
	if (AddSpeed <= 0.0f)
	{
		return;
	}
	const float AccelSpeed = FMath::Min(Accel * InWishSpeed * DeltaTime * SurfaceFriction, AddSpeed);
	Velocity += AccelSpeed * WishDir;
}

void ULambdaPlayerMovement::WalkMove(float DeltaTime)
{
	// CGameMovement::WalkMove
	Accelerate(WishDirection, WishSpeed, SvAccelerate, DeltaTime);

	// Walking does not move you up or down; the ground does that.
	Velocity.Z = 0.0f;
	if (Velocity.IsNearlyZero())
	{
		return;
	}

	const FVector Dest = UpdatedComponent->GetComponentLocation() + Velocity * DeltaTime;
	FHitResult Trace;
	if (!TraceHull(UpdatedComponent->GetComponentLocation(), Dest, Trace))
	{
		UpdatedComponent->SetWorldLocation(Dest, false, nullptr, ETeleportType::TeleportPhysics);
		return;
	}
	// Something in the way: try to step over it.
	StepMove(Dest, Trace);
}

void ULambdaPlayerMovement::AirMove(float DeltaTime)
{
	AirAccelerate(WishDirection, WishSpeed, SvAirAccelerate, DeltaTime);
	TryPlayerMove(nullptr, nullptr);
}

void ULambdaPlayerMovement::StartGravity(float DeltaTime)
{
	// Half the frame's gravity before the move, half after, which is what makes the arc symmetrical.
	const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();
	Velocity.Z -= 0.5f * Settings.GravityUnits * Settings.UnitScale * DeltaTime;
}

void ULambdaPlayerMovement::FinishGravity(float DeltaTime)
{
	const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();
	Velocity.Z -= 0.5f * Settings.GravityUnits * Settings.UnitScale * DeltaTime;
}

bool ULambdaPlayerMovement::CheckJumpButton(float DeltaTime)
{
	// CGameMovement::CheckJumpButton
	if (!bOnGround)
	{
		return false;
	}
	if (bJumpHeldLastFrame)
	{
		return false;	// don't pogo stick
	}
	// "Cannot jump will in the unduck transition": m_bDucking is the transition running and FL_DUCKING is being
	// fully ducked, and Source refuses only when both hold - which is standing back up, not going down. Going
	// down you may jump, and it is a full jump; so may a player who is already ducked.
	if (bDucking && bDucked)
	{
		return false;
	}

	const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();
	const float Scale = Settings.UnitScale;
	// v = sqrt( 2 * g * h ), h being GAMEMOVEMENT_JUMP_HEIGHT.
	const float JumpSpeed = FMath::Sqrt(2.0f * Settings.GravityUnits * Scale
		* Settings.PlayerJumpHeightUnits * Scale);

	// Ducked, Source sets the jump speed rather than adding to it.
	Velocity.Z = JumpSpeed;

	SetGroundActor(nullptr, FHitResult());
	FinishGravity(DeltaTime);
	return true;
}

bool ULambdaPlayerMovement::CanUnDuckHere() const
{
	// Is there room for the standing hull, its feet where they are now?
	const UWorld* World = GetWorld();
	if (!World || !UpdatedComponent)
	{
		return true;
	}
	float Radius, HalfHeight;
	HullDimensions(false, Radius, HalfHeight);
	const float Shrink = 0.5f * ULambdaSourceSettings::Get().UnitScale;
	const FVector Centre = GetFeetLocation() + FVector(0.0f, 0.0f, HalfHeight);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(PlayerUnDuck), false, PawnOwner);
	return !World->OverlapBlockingTestByChannel(Centre, FQuat::Identity, ECC_Pawn,
		FCollisionShape::MakeCapsule(FMath::Max(1.0f, Radius - Shrink), FMath::Max(1.0f, HalfHeight - Shrink)),
		Params);
}

void ULambdaPlayerMovement::FinishDuck()
{
	if (bDucked || !Hull)
	{
		return;
	}
	bDucked = true;
	bDucking = false;
	++DuckChangeCount;
	bLastDuckChangeAirborne = !bOnGround;

	float Radius, HalfHeight;
	HullDimensions(true, Radius, HalfHeight);
	const FVector Before = GetFeetLocation();
	Hull->SetCapsuleSize(Radius, HalfHeight, false);

	if (bOnGround)
	{
		// Standing on something: the feet stay where they are and the head comes down.
		UpdatedComponent->SetWorldLocation(Before + FVector(0.0f, 0.0f, HalfHeight),
			false, nullptr, ETeleportType::TeleportPhysics);
	}
	else
	{
		// In the air the feet come up by the whole difference between the hulls and the head stays, which is
		// what crouch jumping is. Resizing about the centre already lifted them by half; owe the other half.
		UpdatedComponent->AddWorldOffset(FVector(0.0f, 0.0f, HullHalfHeightDelta()),
			false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void ULambdaPlayerMovement::FinishUnDuck()
{
	if (!bDucked || !Hull)
	{
		return;
	}
	bDucked = false;
	bDucking = false;
	++DuckChangeCount;
	bLastDuckChangeAirborne = !bOnGround;

	float Radius, HalfHeight;
	HullDimensions(false, Radius, HalfHeight);
	const FVector Before = GetFeetLocation();
	Hull->SetCapsuleSize(Radius, HalfHeight, false);

	if (bOnGround)
	{
		UpdatedComponent->SetWorldLocation(Before + FVector(0.0f, 0.0f, HalfHeight),
			false, nullptr, ETeleportType::TeleportPhysics);
	}
	else
	{
		// The mirror of the lift, and half of it is again already done by the resize.
		UpdatedComponent->AddWorldOffset(FVector(0.0f, 0.0f, -HullHalfHeightDelta()),
			false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void ULambdaPlayerMovement::Duck(float DeltaTime)
{
	// CGameMovement::Duck, cut to the ducking itself. The view moves across the transition; the hull changes in
	// one step at the end of it.
	if (bWantsToDuck)
	{
		if (!bDucked)
		{
			if (!bDucking)
			{
				bDucking = true;
				DuckTime = 0.0f;
			}
			DuckTime += DeltaTime;
			// SimpleSpline: eased at both ends rather than a straight ramp.
			const float T = FMath::Clamp(DuckTime / TIME_TO_DUCK, 0.0f, 1.0f);
			DuckViewFraction = FMath::SmoothStep(0.0f, 1.0f, T);

			// "Finish ducking immediately if duck time is over or not on ground."
			if (DuckTime > TIME_TO_DUCK || !bOnGround)
			{
				FinishDuck();
				DuckViewFraction = 1.0f;
			}
		}
		else
		{
			DuckViewFraction = 1.0f;
		}
	}
	else if (bDucked || bDucking)
	{
		if (!bDucked)
		{
			// The duck never finished, so there is nothing to undo but the view.
			bDucking = false;
			DuckTime = 0.0f;
			DuckViewFraction = 0.0f;
			return;
		}
		if (!CanUnDuckHere())
		{
			return;		// something overhead; stay down until there is room
		}
		if (!bDucking)
		{
			bDucking = true;
			DuckTime = 0.0f;
		}
		DuckTime += DeltaTime;
		const float T = FMath::Clamp(DuckTime / TIME_TO_UNDUCK, 0.0f, 1.0f);
		DuckViewFraction = 1.0f - FMath::SmoothStep(0.0f, 1.0f, T);

		if (DuckTime > TIME_TO_UNDUCK || !bOnGround)
		{
			FinishUnDuck();
			DuckViewFraction = 0.0f;
		}
	}
	else
	{
		DuckViewFraction = 0.0f;
	}
}

void ULambdaPlayerMovement::FullWalkMove(float DeltaTime)
{
	// CGameMovement::FullWalkMove
	StartGravity(DeltaTime);

	if (bJumpPressed)
	{
		CheckJumpButton(DeltaTime);
	}

	if (bOnGround)
	{
		Velocity.Z = 0.0f;
		ApplyFriction(DeltaTime);
	}

	if (bOnGround)
	{
		WalkMove(DeltaTime);
	}
	else
	{
		AirMove(DeltaTime);
	}

	CategorizePosition();
	FinishGravity(DeltaTime);

	if (bOnGround)
	{
		Velocity.Z = 0.0f;
	}
}

void ULambdaPlayerMovement::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!PawnOwner || !UpdatedComponent || ShouldSkipUpdate(DeltaTime) || DeltaTime < UE_SMALL_NUMBER)
	{
		ConsumeInputVector();
		return;
	}

	// The wish direction and how hard it is being asked for, from whatever drove AddMovementInput this frame.
	const FVector Input = ConsumeInputVector().GetClampedToMaxSize(1.0f);
	WishDirection = FVector(Input.X, Input.Y, 0.0f).GetSafeNormal();
	WishSpeed = MaxSpeedCm * FVector(Input.X, Input.Y, 0.0f).Size();

	// Before anything else: if the hull is inside something, walk it out, or every move this frame begins solid
	// and does nothing at all.
	NudgePosition();

	CategorizePosition();
	Duck(DeltaTime);
	FullWalkMove(DeltaTime);

	bJumpHeldLastFrame = bJumpPressed;
	bJumpPressed = false;

	UpdateComponentVelocity();
}
