#include "SourceFuncDoorRotating.h"
#include "LambdaSourceModule.h"
#include "LambdaSourceSettings.h"
#include "SourceCoordinates.h"
#include "ProceduralMeshComponent.h"
#include "LambdaSoundLibrary.h"
#include "Components/AudioComponent.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"

using namespace SourceDoorFlags;

ASourceFuncDoorRotating::ASourceFuncDoorRotating()
{
	PrimaryActorTick.bCanEverTick = true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Spawn - CRotDoor::Spawn + the parts of CBaseDoor::Spawn that apply to rotating doors
// ---------------------------------------------------------------------------------------------------------------------

void ASourceFuncDoorRotating::InitializeFromEntity(const FSourceBSPFile& Map, int32 ModelIndex, const FSourceEntity& InEntity,
	ULambdaMaterialLibrary* MaterialLibrary, ASourceBSPWorldActor* InWorldActor)
{
	Super::InitializeFromEntity(Map, ModelIndex, InEntity, MaterialLibrary, InWorldActor);

	// CBaseToggle::KeyValue / CBaseDoor::Spawn defaults.
	MoveDistance = Entity.GetFloat(TEXT("distance"), 90.0f);
	if (MoveDistance == 0.0f)
	{
		MoveDistance = 90.0f;
	}
	Speed = Entity.GetFloat(TEXT("speed"), 100.0f);
	if (Speed == 0.0f)
	{
		Speed = 100.0f;	// CBaseDoor::Spawn: if (m_flSpeed == 0) m_flSpeed = 100;
	}
	Wait = Entity.GetFloat(TEXT("wait"), 4.0f);
	SpawnPosition = (ESourceDoorSpawnPos)Entity.GetInt(TEXT("spawnpos"), 0);

	LockedSound = Entity.Get(TEXT("locked_sound"));
	UnlockedSound = Entity.Get(TEXT("unlocked_sound"));
	BlockDamage = Entity.GetFloat(TEXT("dmg"), 0.0f);
	bForceClosed = Entity.GetInt(TEXT("forceclosed"), 0) != 0;

	NoiseMoving = Entity.Get(TEXT("noise1"));
	NoiseArrived = Entity.Get(TEXT("noise2"));
	NoiseMovingClosed = Entity.Get(TEXT("startclosesound"));
	NoiseArrivedClosed = Entity.Get(TEXT("closesound"));

	if (HasSpawnFlags(SF_DOOR_LOCKED))
	{
		bLocked = true;
	}

	// CBaseToggle::AxisDir - pick the rotation axis.
	AxisDir();

	// check for clockwise rotation
	if (HasSpawnFlags(SF_DOOR_ROTATE_BACKWARDS))
	{
		MoveAng = MoveAng * -1.0f;
	}

	Angle1 = SourceAngles;
	Angle2 = SourceAngles + MoveAng * MoveDistance;

	if (Angle1.Equals(Angle2))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("func_door_rotating: start/end positions are equal (distance=%g)"), MoveDistance);
	}

	if (HasSpawnFlags(SF_DOOR_START_OPEN_OBSOLETE))
	{
		// swap pos1 and pos2, put door at pos2, invert movement direction
		const FVector3f NewAngles = Angle2;
		Angle2 = Angle1;
		Angle1 = NewAngles;
		MoveAng = -MoveAng;

		SetSourceAngles(Angle1);
		ToggleState = ESourceToggleState::AtBottom;
	}
	else if (SpawnPosition == ESourceDoorSpawnPos::Open)
	{
		SetSourceAngles(Angle2);
		ToggleState = ESourceToggleState::AtTop;
	}
	else
	{
		ToggleState = ESourceToggleState::AtBottom;
	}

	// SF_DOOR_PASSABLE doors are non-solid (CBaseDoor::Spawn adds FSOLID_NOT_SOLID).
	if (HasSpawnFlags(SF_DOOR_PASSABLE))
	{
		BrushMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	UE_LOG(LogLambdaSource, Log, TEXT("  func_door_rotating: speed=%g distance=%g wait=%g axis=%s angle1=%s angle2=%s state=%s%s%s"),
		Speed, MoveDistance, Wait, *MoveAng.ToString(), *Angle1.ToString(), *Angle2.ToString(),
		ToggleState == ESourceToggleState::AtTop ? TEXT("open") : TEXT("closed"),
		HasSpawnFlags(SF_DOOR_PUSE) ? TEXT(" +USE") : TEXT(""),
		HasSpawnFlags(SF_DOOR_NO_AUTO_RETURN) ? TEXT(" toggle") : TEXT(""));
}

void ASourceFuncDoorRotating::AxisDir()
{
	// CBaseToggle::AxisDir
	if (HasSpawnFlags(SF_DOOR_ROTATE_ROLL))
	{
		MoveAng = FVector3f(0.0f, 0.0f, 1.0f);	// angles are roll
	}
	else if (HasSpawnFlags(SF_DOOR_ROTATE_PITCH))
	{
		MoveAng = FVector3f(1.0f, 0.0f, 0.0f);	// angles are pitch
	}
	else
	{
		MoveAng = FVector3f(0.0f, 1.0f, 0.0f);	// angles are yaw
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Use
// ---------------------------------------------------------------------------------------------------------------------

bool ASourceFuncDoorRotating::IsUsable() const
{
	// CBaseDoor::ObjectCaps - only doors flagged SF_DOOR_PUSE take +USE (and never when SF_DOOR_IGNORE_USE).
	return HasSpawnFlags(SF_DOOR_PUSE) && !HasSpawnFlags(SF_DOOR_IGNORE_USE);
}

void ASourceFuncDoorRotating::OnUsed(AActor* InActivator)
{
	// CBaseDoor::Use
	Activator = InActivator;

	if (bLocked)
	{
		// CBaseDoor::Use / DoorTouch: locked doors report and play the locked sound.
		FireOutput(TEXT("OnLockedUse"), InActivator);
		PlayLockSounds(true);
		return;
	}

	// Doors that are not "toggle" doors only respond to +USE while closed, unless SF_DOOR_USE_CLOSES lets the player
	// shut them early (CBaseDoor::Use).
	if (ToggleState == ESourceToggleState::AtTop && !HasSpawnFlags(SF_DOOR_NO_AUTO_RETURN) && !HasSpawnFlags(SF_DOOR_USE_CLOSES))
	{
		return;
	}

	DoorActivate();
}

int32 ASourceFuncDoorRotating::DoorActivate()
{
	// CBaseDoor::DoorActivate
	if (HasSpawnFlags(SF_DOOR_NO_AUTO_RETURN) && ToggleState == ESourceToggleState::AtTop)
	{
		// door should close
		DoorGoDown();
	}
	else
	{
		// door should open
		if (ToggleState != ESourceToggleState::AtTop && ToggleState != ESourceToggleState::GoingUp)
		{
			DoorGoUp();
		}
	}
	return 1;
}

void ASourceFuncDoorRotating::DoorTouch(AActor* Other)
{
	// CBaseDoor::DoorTouch
	if (!Other)
	{
		return;
	}
	// If door is not opened by touch, do nothing.
	if (!HasSpawnFlags(SF_DOOR_PTOUCH))
	{
		return;
	}
	if (bLocked)
	{
		FireOutput(TEXT("OnLockedUse"), Other);
		PlayLockSounds(true);
		return;
	}

	// Remember who activated the door.
	Activator = Other;

	if (DoorActivate())
	{
		// Temporarily disable the touch function, until movement is finished.
		bTouchEnabled = false;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Inputs - CBaseDoor's DEFINE_INPUTFUNC list
// ---------------------------------------------------------------------------------------------------------------------

bool ASourceFuncDoorRotating::AcceptInput(const FString& InputName, AActor* InActivator, AActor* Caller, const FString& Parameter)
{
	if (InputName.Equals(TEXT("Open"), ESearchCase::IgnoreCase))
	{
		Activator = InActivator;
		InputOpen();
		return true;
	}
	if (InputName.Equals(TEXT("Close"), ESearchCase::IgnoreCase))
	{
		Activator = InActivator;
		InputClose();
		return true;
	}
	if (InputName.Equals(TEXT("Toggle"), ESearchCase::IgnoreCase))
	{
		Activator = InActivator;
		InputToggle();
		return true;
	}
	if (InputName.Equals(TEXT("Lock"), ESearchCase::IgnoreCase))
	{
		Lock();
		return true;
	}
	if (InputName.Equals(TEXT("Unlock"), ESearchCase::IgnoreCase))
	{
		Unlock();
		return true;
	}
	if (InputName.Equals(TEXT("SetSpeed"), ESearchCase::IgnoreCase))
	{
		Speed = FCString::Atof(*Parameter);
		return true;
	}
	if (InputName.Equals(TEXT("SetToggleState"), ESearchCase::IgnoreCase))
	{
		SetToggleState(FCString::Atoi(*Parameter) == 1 ? ESourceToggleState::AtTop : ESourceToggleState::AtBottom);
		return true;
	}
	if (InputName.Equals(TEXT("Use"), ESearchCase::IgnoreCase))
	{
		OnUsed(InActivator);
		return true;
	}
	return Super::AcceptInput(InputName, InActivator, Caller, Parameter);
}

void ASourceFuncDoorRotating::InputOpen()
{
	// CBaseDoor::InputOpen
	if (ToggleState != ESourceToggleState::AtTop && ToggleState != ESourceToggleState::GoingUp)
	{
		// I'm locked, can't open
		if (bLocked)
		{
			return;
		}
		// Play door unlock sounds.
		PlayLockSounds(false);
		DoorGoUp();
	}
}

void ASourceFuncDoorRotating::InputClose()
{
	// CBaseDoor::InputClose
	if (ToggleState != ESourceToggleState::AtBottom)
	{
		DoorGoDown();
	}
}

void ASourceFuncDoorRotating::InputToggle()
{
	// CBaseDoor::InputToggle
	if (bLocked)
	{
		return;
	}
	if (ToggleState == ESourceToggleState::AtBottom)
	{
		DoorGoUp();
	}
	else if (ToggleState == ESourceToggleState::AtTop)
	{
		DoorGoDown();
	}
}

void ASourceFuncDoorRotating::SetToggleState(ESourceToggleState State)
{
	// CRotDoor::SetToggleState
	SetSourceAngles(State == ESourceToggleState::AtTop ? Angle2 : Angle1);
	ToggleState = State;
	AngularVelocity = FVector3f::ZeroVector;
	MoveDoneTime = -1.0f;
	PendingMove = EPendingMove::None;
}

void ASourceFuncDoorRotating::PlayLockSounds(bool bLockedSound)
{
	const FString& SoundName = bLockedSound ? LockedSound : UnlockedSound;
	if (SoundName.IsEmpty() || SoundName == TEXT("0"))
	{
		return;
	}
	if (ULambdaSoundWave* Wave = FLambdaSoundCache::Get().CreateWave(this, SoundName, false))
	{
		UGameplayStatics::SpawnSoundAtLocation(this, Wave, GetActorLocation());
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Blocked - CBaseDoor::StartBlocked / Blocked / EndBlocked
// ---------------------------------------------------------------------------------------------------------------------

void ASourceFuncDoorRotating::CheckBlocked()
{
	// Source detects this while pushing the door through the physics system; here we test the moving door's box
	// against players each frame, which catches the same case: something standing in the doorway.
	AActor* Found = nullptr;
	const FBox DoorBox = BrushMesh->Bounds.GetBox();
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APawn* Pawn = It->IsValid() ? It->Get()->GetPawn() : nullptr;
		if (Pawn && DoorBox.Intersect(Pawn->GetComponentsBoundingBox()))
		{
			Found = Pawn;
			break;
		}
	}

	if (Found && !Blocker.IsValid())
	{
		Blocker = Found;
		StartBlocked(Found);
		Blocked(Found);
	}
	else if (!Found && Blocker.IsValid())
	{
		Blocker = nullptr;
		EndBlocked();
	}
}

void ASourceFuncDoorRotating::StartBlocked(AActor* Other)
{
	// CBaseDoor::StartBlocked
	if (ToggleState == ESourceToggleState::GoingDown)
	{
		FireOutput(TEXT("OnBlockedClosing"), Other);
	}
	else
	{
		FireOutput(TEXT("OnBlockedOpening"), Other);
	}
}

void ASourceFuncDoorRotating::Blocked(AActor* Other)
{
	// CBaseDoor::Blocked - if we're set to force ourselves closed, keep going.
	if (bForceClosed)
	{
		return;
	}

	// A door with a negative wait would never come back if blocked, so it just keeps going.
	if (Wait >= 0.0f)
	{
		if (ToggleState == ESourceToggleState::GoingDown)
		{
			DoorGoUp();
		}
		else
		{
			DoorGoDown();
		}
	}
}

void ASourceFuncDoorRotating::EndBlocked()
{
	// CBaseDoor::EndBlocked
	if (ToggleState == ESourceToggleState::GoingDown)
	{
		FireOutput(TEXT("OnUnblockedClosing"), nullptr);
	}
	else
	{
		FireOutput(TEXT("OnUnblockedOpening"), nullptr);
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Sounds - CBaseDoor::StartMovingSound / StopMovingSound and the arrival sounds in DoorHitTop/DoorHitBottom
// ---------------------------------------------------------------------------------------------------------------------

void ASourceFuncDoorRotating::StartMovingSound()
{
	if (HasSpawnFlags(SF_DOOR_SILENT))
	{
		return;
	}
	// MovingSoundThink: noise1 normally, startclosesound while closing if one was given.
	const bool bClosing = (ToggleState == ESourceToggleState::GoingDown || ToggleState == ESourceToggleState::AtBottom);
	const FString& SoundName = (NoiseMovingClosed.IsEmpty() || !bClosing) ? NoiseMoving : NoiseMovingClosed;
	if (SoundName.IsEmpty())
	{
		return;
	}

	StopMovingSound();
	if (ULambdaSoundWave* Wave = FLambdaSoundCache::Get().CreateWave(this, SoundName, /*bLoop=*/ true))
	{
		MovingAudio = UGameplayStatics::SpawnSoundAtLocation(this, Wave, GetActorLocation());
	}
}

void ASourceFuncDoorRotating::StopMovingSound()
{
	if (MovingAudio)
	{
		MovingAudio->Stop();
		MovingAudio = nullptr;
	}
}

void ASourceFuncDoorRotating::PlayArrivedSound()
{
	if (HasSpawnFlags(SF_DOOR_SILENT))
	{
		return;
	}
	// DoorHitBottom prefers closesound when one is set; DoorHitTop always uses noise2.
	const bool bClosed = (ToggleState == ESourceToggleState::AtBottom);
	const FString& SoundName = (bClosed && !NoiseArrivedClosed.IsEmpty()) ? NoiseArrivedClosed : NoiseArrived;
	if (SoundName.IsEmpty())
	{
		return;
	}
	if (ULambdaSoundWave* Wave = FLambdaSoundCache::Get().CreateWave(this, SoundName, /*bLoop=*/ false))
	{
		UGameplayStatics::SpawnSoundAtLocation(this, Wave, GetActorLocation());
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------------------------------------------------

void ASourceFuncDoorRotating::DoorGoUp()
{
	// CBaseDoor::DoorGoUp (rotating branch)
	// "If we're not moving already, start the moving noise"
	if (ToggleState != ESourceToggleState::GoingUp && ToggleState != ESourceToggleState::GoingDown)
	{
		StartMovingSound();
	}
	ToggleState = ESourceToggleState::GoingUp;
	PendingMove = EPendingMove::HitTop;

	float Sign = 1.0f;
	if (Activator.IsValid() && !HasSpawnFlags(SF_DOOR_ONEWAY) && MoveAng.Y != 0.0f)
	{
		// Y axis rotation, move away from the player.
		// "Point right hand at door hinge, curl hand towards closest spot on door, if thumb is up, open door CW."
		// Done in Source space so the cross-product sign matches the original.
		const float Scale = ULambdaSourceSettings::Get().UnitScale;
		const FVector ActivatorUE = Activator->GetActorLocation();
		const FVector3f ActivatorSrc = FSourceCoords::ToSource(ActivatorUE, Scale);

		// CollisionProp()->CalcNearestPoint( activator ) - nearest point on the door's box to the activator.
		const FVector NearestUE = BrushMesh->Bounds.GetBox().GetClosestPointTo(ActivatorUE);
		const FVector3f NearestSrc = FSourceCoords::ToSource(NearestUE, Scale);

		FVector3f ActivatorToNearestPoint = NearestSrc - ActivatorSrc;
		ActivatorToNearestPoint.Z = 0.0f;

		FVector3f ActivatorToOrigin = SourceOrigin - ActivatorSrc;
		ActivatorToOrigin.Z = 0.0f;

		const float NearestPointDistToOrigin = (ActivatorToOrigin - ActivatorToNearestPoint).Size();
		ActivatorToOrigin.Normalize();
		ActivatorToNearestPoint.Normalize();
		const float Dot = FVector3f::DotProduct(ActivatorToOrigin, ActivatorToNearestPoint);
		if (NearestPointDistToOrigin < 5.0f && Dot > 0.99f)
		{
			// Nearest point and origin are nearly the same and in line - pull the origin out from the centre so the
			// cross product is well defined.
			const FVector3f CenterSrc = FSourceCoords::ToSource(BrushMesh->Bounds.Origin, Scale);
			FVector3f Origin = SourceOrigin + (SourceOrigin - CenterSrc);
			ActivatorToOrigin = Origin - ActivatorSrc;
			ActivatorToOrigin.Z = 0.0f;
			ActivatorToOrigin.Normalize();
		}

		const FVector3f Cross = FVector3f::CrossProduct(ActivatorToOrigin, ActivatorToNearestPoint);
		if (Cross.Z > 0.0f)
		{
			Sign = -1.0f;
		}
	}

	AngularMove(Angle2 * Sign, Speed);

	// Fire our open output.
	FireOutput(TEXT("OnOpen"), Activator.Get());
}

void ASourceFuncDoorRotating::DoorHitTop()
{
	// CBaseDoor::DoorHitTop
	StopMovingSound();
	ToggleState = ESourceToggleState::AtTop;
	PlayArrivedSound();

	if (HasSpawnFlags(SF_DOOR_NO_AUTO_RETURN))
	{
		// toggle-doors don't come down automatically, they wait for refire.
		// Re-instate touch method, movement is complete.
		bTouchEnabled = true;
		PendingMove = EPendingMove::None;
		MoveDoneTime = -1.0f;
	}
	else
	{
		// In flWait seconds, DoorGoDown will fire, unless wait is -1, then door stays open.
		if (Wait == -1.0f)
		{
			PendingMove = EPendingMove::None;
			MoveDoneTime = -1.0f;
		}
		else
		{
			PendingMove = EPendingMove::GoDown;
			SetMoveDoneTime(Wait);
		}
	}
	// SF_DOOR_START_OPEN_OBSOLETE swaps the meaning of the two ends.
	FireOutput(HasSpawnFlags(SF_DOOR_START_OPEN_OBSOLETE) ? TEXT("OnFullyClosed") : TEXT("OnFullyOpen"), Activator.Get());
	UE_LOG(LogLambdaSource, Verbose, TEXT("func_door_rotating: fully open"));
}

void ASourceFuncDoorRotating::DoorGoDown()
{
	// CBaseDoor::DoorGoDown
	if (ToggleState != ESourceToggleState::GoingUp && ToggleState != ESourceToggleState::GoingDown)
	{
		StartMovingSound();
	}
	ToggleState = ESourceToggleState::GoingDown;
	PendingMove = EPendingMove::HitBottom;
	AngularMove(Angle1, Speed);

	// Fire our closed output.
	FireOutput(TEXT("OnClose"), Activator.Get());
}

void ASourceFuncDoorRotating::DoorHitBottom()
{
	// CBaseDoor::DoorHitBottom
	StopMovingSound();
	ToggleState = ESourceToggleState::AtBottom;
	PlayArrivedSound();

	// Re-instate touch method, cycle is complete.
	bTouchEnabled = true;
	PendingMove = EPendingMove::None;
	MoveDoneTime = -1.0f;
	FireOutput(HasSpawnFlags(SF_DOOR_START_OPEN_OBSOLETE) ? TEXT("OnFullyOpen") : TEXT("OnFullyClosed"), Activator.Get());
	UE_LOG(LogLambdaSource, Verbose, TEXT("func_door_rotating: fully closed"));
}

// ---------------------------------------------------------------------------------------------------------------------
// CBaseToggle::AngularMove
// ---------------------------------------------------------------------------------------------------------------------

void ASourceFuncDoorRotating::AngularMove(const FVector3f& DestAngle, float InSpeed)
{
	FinalAngle = DestAngle;

	// Already there?
	if (DestAngle.Equals(SourceAngles))
	{
		MoveDone();
		return;
	}

	// set destdelta to the vector needed to move
	const FVector3f DestDelta = DestAngle - SourceAngles;

	// divide by speed to get time to reach dest
	float TravelTime = AngleLength(DestDelta) / InSpeed;

	const float MinTravelTime = 0.01f;
	if (TravelTime < MinTravelTime)
	{
		TravelTime = MinTravelTime;
	}

	SetMoveDoneTime(TravelTime);

	// scale the destdelta vector by the time spent traveling to get velocity
	AngularVelocity = DestDelta * (1.0f / TravelTime);
}

void ASourceFuncDoorRotating::AngularMoveDone()
{
	// CBaseToggle::AngularMoveDone
	SetSourceAngles(FinalAngle);
	AngularVelocity = FVector3f::ZeroVector;
	MoveDoneTime = -1.0f;
}

void ASourceFuncDoorRotating::SetMoveDoneTime(float Delay)
{
	MoveDoneTime = Delay;
}

void ASourceFuncDoorRotating::MoveDone()
{
	const EPendingMove Pending = PendingMove;
	PendingMove = EPendingMove::None;

	switch (Pending)
	{
	case EPendingMove::HitTop:
		AngularMoveDone();
		DoorHitTop();
		break;
	case EPendingMove::HitBottom:
		AngularMoveDone();
		DoorHitBottom();
		break;
	case EPendingMove::GoDown:
		MoveDoneTime = -1.0f;
		DoorGoDown();
		break;
	default:
		MoveDoneTime = -1.0f;
		break;
	}
}

void ASourceFuncDoorRotating::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Touch links: Source fires Touch when the player's collision box overlaps the entity's. Only players open doors.
	if (bTouchEnabled && HasSpawnFlags(SF_DOOR_PTOUCH))
	{
		const FBox DoorBox = BrushMesh->Bounds.GetBox();
		for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
		{
			APawn* Pawn = It->IsValid() ? It->Get()->GetPawn() : nullptr;
			if (Pawn && DoorBox.Intersect(Pawn->GetComponentsBoundingBox()))
			{
				DoorTouch(Pawn);
				break;
			}
		}
	}

	if (MoveDoneTime < 0.0f)
	{
		return;
	}

	// While pushing, watch for anything caught in the doorway.
	if (ToggleState == ESourceToggleState::GoingUp || ToggleState == ESourceToggleState::GoingDown)
	{
		CheckBlocked();
	}

	// MOVETYPE_PUSH: integrate angular velocity, then fire MoveDone when the move time elapses.
	const bool bMoving = !AngularVelocity.IsNearlyZero();
	const float Step = FMath::Min(DeltaSeconds, MoveDoneTime);
	if (bMoving && Step > 0.0f)
	{
		SetSourceAngles(SourceAngles + AngularVelocity * Step);
	}

	MoveDoneTime -= DeltaSeconds;
	if (MoveDoneTime <= 0.0f)
	{
		MoveDone();
	}
}
