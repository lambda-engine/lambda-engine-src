#include "LambdaPlayerMovement.h"

#include "Core/LambdaSourceSettings.h"

#include "Components/BoxComponent.h"
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
	return Hull ? Hull->GetScaledBoxExtent().Z * 2.0f : 0.0f;
}

FVector ULambdaPlayerMovement::GetFeetLocation() const
{
	if (!UpdatedComponent)
	{
		return FVector::ZeroVector;
	}
	return UpdatedComponent->GetComponentLocation() - FVector(0.0f, 0.0f, GetHullHeight() * 0.5f);
}

FVector ULambdaPlayerMovement::HullHalfExtent(bool bDuckedHull) const
{
	// VEC_HULL_MIN/MAX: 32 wide and 72 tall standing, 36 tall ducked.
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	return FVector(16.0f * Scale, 16.0f * Scale, (bDuckedHull ? 18.0f : 36.0f) * Scale);
}

bool ULambdaPlayerMovement::TraceHull(const FVector& Start, const FVector& End, FHitResult& OutHit) const
{
	const UWorld* World = GetWorld();
	if (!World || !Hull)
	{
		return false;
	}
	// TracePlayerBBox: the player's own box, swept. Shrunk a hair so a box resting exactly on a surface does not
	// report itself as already touching it, which is what DIST_EPSILON is for in Source.
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	FVector Extent = Hull->GetScaledBoxExtent();
	Extent -= FVector(DIST_EPSILON * Scale);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(PlayerHull), /*bTraceComplex=*/ false, PawnOwner);
	Params.bReturnFaceIndex = true;
	return World->SweepSingleByChannel(OutHit, Start, End, FQuat::Identity, ECC_Pawn,
		FCollisionShape::MakeBox(Extent), Params);
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
	// Settle onto it, so the box rests on the surface rather than a fraction above it.
	if (Hit.bBlockingHit && Hit.Distance > 0.0f)
	{
		UpdatedComponent->SetWorldLocation(Hit.Location, false, nullptr, ETeleportType::TeleportPhysics);
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
	// Is there room for the standing hull where we are?
	if (!UpdatedComponent)
	{
		return true;
	}
	const UWorld* World = GetWorld();
	if (!World)
	{
		return true;
	}
	const FVector StandExtent = HullHalfExtent(false);
	// The feet stay put on the ground, so the standing box grows upwards from them.
	const FVector Feet = GetFeetLocation();
	const FVector Centre = Feet + FVector(0.0f, 0.0f, StandExtent.Z);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(PlayerUnDuck), false, PawnOwner);
	return !World->OverlapBlockingTestByChannel(Centre, FQuat::Identity, ECC_Pawn,
		FCollisionShape::MakeBox(StandExtent - FVector(DIST_EPSILON)), Params);
}

void ULambdaPlayerMovement::FinishDuck()
{
	if (bDucked || !Hull)
	{
		return;
	}
	bDucked = true;
	bDucking = false;

	const FVector Before = GetFeetLocation();
	Hull->SetBoxExtent(HullHalfExtent(true), false);

	if (bOnGround)
	{
		// Standing on something: the feet stay where they are and the head comes down.
		const FVector Centre = Before + FVector(0.0f, 0.0f, HullHalfExtent(true).Z);
		UpdatedComponent->SetWorldLocation(Centre, false, nullptr, ETeleportType::TeleportPhysics);
	}
	else
	{
		// In the air: the feet come up by the whole difference between the hulls and the head stays. This is
		// what crouch jumping is.
		const FVector Lift = FVector(0.0f, 0.0f, HullHalfExtent(false).Z - HullHalfExtent(true).Z);
		UpdatedComponent->AddWorldOffset(Lift * 2.0f, false, nullptr, ETeleportType::TeleportPhysics);
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

	const FVector Before = GetFeetLocation();
	Hull->SetBoxExtent(HullHalfExtent(false), false);

	if (bOnGround)
	{
		const FVector Centre = Before + FVector(0.0f, 0.0f, HullHalfExtent(false).Z);
		UpdatedComponent->SetWorldLocation(Centre, false, nullptr, ETeleportType::TeleportPhysics);
	}
	else
	{
		const FVector Drop = FVector(0.0f, 0.0f, HullHalfExtent(false).Z - HullHalfExtent(true).Z);
		UpdatedComponent->AddWorldOffset(-Drop * 2.0f, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void ULambdaPlayerMovement::Duck(float DeltaTime)
{
	// CGameMovement::Duck, cut to the ducking itself.
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
			// "Finish ducking immediately if duck time is over or not on ground."
			if (DuckTime > TIME_TO_DUCK || !bOnGround)
			{
				FinishDuck();
			}
		}
	}
	else if (bDucked || bDucking)
	{
		if (!CanUnDuckHere())
		{
			return;		// something overhead; stay down until there is room
		}
		if (!bDucked)
		{
			bDucking = false;	// the duck never finished, so there is nothing to undo
			return;
		}
		DuckTime += DeltaTime;
		if (DuckTime > TIME_TO_UNDUCK || !bOnGround)
		{
			FinishUnDuck();
		}
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

	CategorizePosition();
	Duck(DeltaTime);
	FullWalkMove(DeltaTime);

	bJumpHeldLastFrame = bJumpPressed;
	bJumpPressed = false;

	UpdateComponentVelocity();
}
