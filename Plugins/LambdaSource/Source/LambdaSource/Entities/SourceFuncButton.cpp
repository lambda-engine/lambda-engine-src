#include "Entities/SourceFuncButton.h"
#include "Audio/LambdaSoundLibrary.h"
#include "Audio/SourceSoundScript.h"
#include "Core/LambdaSourceModule.h"
#include "Core/LambdaSourceSettings.h"
#include "Core/SourceCoordinates.h"
#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "ProceduralMeshComponent.h"

using namespace SourceButtonFlags;

ASourceFuncButton::ASourceFuncButton()
{
	PrimaryActorTick.bCanEverTick = true;
}

// ---------------------------------------------------------------------------------------------------------------------
// CBaseButton::Spawn
// ---------------------------------------------------------------------------------------------------------------------

void ASourceFuncButton::InitializeFromEntity(const FSourceBSPFile& Map, int32 ModelIndex, const FSourceEntity& InEntity,
	ULambdaMaterialLibrary* MaterialLibrary, ASourceBSPWorldActor* InWorldActor)
{
	Super::InitializeFromEntity(Map, ModelIndex, InEntity, MaterialLibrary, InWorldActor);

	// Each sound is named outright - a wav under sound/, or a soundscript entry. The old numbered keyvalues are
	// still read as a fallback, so a map built before this change keeps the sounds its author chose; Source's
	// numbered set is reachable either way, since MakeButtonSound( n ) is only ever "Buttons.snd<n>".
	auto NumberedSound = [this](const TCHAR* Key) -> FString
	{
		const int32 Number = Entity.GetInt(Key, 0);
		return Number != 0 ? FString::Printf(TEXT("Buttons.snd%d"), Number) : FString();
	};

	PressInSound = Entity.Get(TEXT("press_in_sound"));
	if (PressInSound.IsEmpty())
	{
		PressInSound = NumberedSound(TEXT("sounds"));
	}
	// Left blank means "the same going out as coming in", which is what Source did with its single sound and is
	// what most buttons want. Resolved here rather than at each use, so the two directions cannot drift apart.
	PressOutSound = Entity.Get(TEXT("press_out_sound"));
	if (PressOutSound.IsEmpty())
	{
		PressOutSound = PressInSound;
	}

	UseLockedSound = Entity.Get(TEXT("use_locked_sound"));
	if (UseLockedSound.IsEmpty())
	{
		UseLockedSound = NumberedSound(TEXT("locked_sound"));
	}
	LockSound = Entity.Get(TEXT("lock_sound"));
	UnlockSound = Entity.Get(TEXT("unlock_sound"));
	if (UnlockSound.IsEmpty())
	{
		UnlockSound = NumberedSound(TEXT("unlocked_sound"));
	}

	// Convert movedir from angles to a vector (CBaseButton::Spawn: AngleVectors( angMoveDir, &m_vecMoveDir )).
	FVector3f MoveAngles = FVector3f::ZeroVector;
	Entity.GetVector(TEXT("movedir"), MoveAngles);
	MoveDir = FSourceCoords::AngleVectorsForward(MoveAngles);

	if (HasSpawnFlags(SF_BUTTON_NOTSOLID))
	{
		BrushMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	Speed = Entity.GetFloat(TEXT("speed"), 0.0f);
	if (Speed == 0.0f)
	{
		Speed = 40.0f;
	}
	Wait = Entity.GetFloat(TEXT("wait"), 0.0f);
	if (Wait == 0.0f)
	{
		Wait = 1.0f;
	}
	// NOTE: cstrike15's CBaseButton::Spawn (buttons.cpp:422) forces lip to 4 when it is 0, which would give a
	// 4-unit-deep button zero travel. We use the authored value so lip 0 means "move the full depth", which is the
	// documented func_button behaviour and what Hammer implies.
	Lip = Entity.GetFloat(TEXT("lip"), 0.0f);

	ToggleState = ESourceToggleState::AtBottom;
	Position1 = SourceOrigin;

	// m_vecPosition2 = m_vecPosition1 + movedir * (|movedir . OBBSize| - lip)
	// The brush mesh is built around the entity origin, so its local bounds are the OBB, in UE units.
	// Source computes this from CollisionProp()->OBBSize() and then subtracts 2, because its model loader grows
	// brush-model bounds by 1 in every direction (Mod_LoadSubmodels). Our mesh bounds come straight from the BSP
	// geometry and are never grown, so they already equal Source's (OBBSize - 2) and must not be shrunk again.
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const FVector LocalSizeUE = LocalBounds.GetSize();
	const FVector3f OBBSize(
		(float)FMath::Abs(LocalSizeUE.X) / Scale,
		(float)FMath::Abs(LocalSizeUE.Y) / Scale,
		(float)FMath::Abs(LocalSizeUE.Z) / Scale);

	// DotProductAbs( m_vecMoveDir, vecButtonOBB )
	const float MoveAmount = FMath::Abs(MoveDir.X * OBBSize.X) + FMath::Abs(MoveDir.Y * OBBSize.Y) + FMath::Abs(MoveDir.Z * OBBSize.Z);
	Position2 = Position1 + MoveDir * (MoveAmount - Lip);

	// Is this a non-moving button?
	if ((Position2 - Position1).Size() < 1.0f || HasSpawnFlags(SF_BUTTON_DONTMOVE))
	{
		Position2 = Position1;
	}

	bStayPushed = (Wait == -1.0f);

	if (HasSpawnFlags(SF_BUTTON_LOCKED))
	{
		bLocked = true;
	}
	bTouchEnabled = HasSpawnFlags(SF_BUTTON_TOUCH_ACTIVATES);

	UE_LOG(LogLambdaSource, Log, TEXT("  func_button '%s': speed=%g wait=%g lip=%g movedir=%s pos1=%s pos2=%s%s%s%s"),
		*TargetName, Speed, Wait, Lip, *MoveDir.ToString(), *Position1.ToString(), *Position2.ToString(),
		HasSpawnFlags(SF_BUTTON_USE_ACTIVATES) ? TEXT(" +USE") : TEXT(""),
		HasSpawnFlags(SF_BUTTON_TOUCH_ACTIVATES) ? TEXT(" touch") : TEXT(""),
		bLocked ? TEXT(" locked") : TEXT(""));
}

// ---------------------------------------------------------------------------------------------------------------------
// Use / touch
// ---------------------------------------------------------------------------------------------------------------------

bool ASourceFuncButton::IsUsable() const
{
	// CBaseButton::ObjectCaps - only +USE buttons are usable.
	return HasSpawnFlags(SF_BUTTON_USE_ACTIVATES);
}

void ASourceFuncButton::OnUsed(AActor* InActivator)
{
	ButtonUse(InActivator);
}

void ASourceFuncButton::ButtonUse(AActor* InActivator)
{
	// CBaseButton::ButtonUse
	// Ignore touches if button is moving, or pushed-in and waiting to auto-come-out.
	if (ToggleState == ESourceToggleState::GoingUp || ToggleState == ESourceToggleState::GoingDown)
	{
		return;
	}

	if (bLocked)
	{
		OnUseLocked(InActivator);
		return;
	}

	Activator = InActivator;

	if (ToggleState == ESourceToggleState::AtTop)
	{
		// If it's a toggle button it can return now. Otherwise it will either return on its own or stay pressed.
		if (HasSpawnFlags(SF_BUTTON_TOGGLE))
		{
			PlaySoundField(PressOutSound, false);
			FireOutput(TEXT("OnPressed"), Activator.Get());
			ButtonReturn();
		}
	}
	else
	{
		FireOutput(TEXT("OnPressed"), Activator.Get());
		ButtonActivate();
	}
}

void ASourceFuncButton::ButtonTouch(AActor* Other)
{
	// CBaseButton::ButtonTouch - only players, and only when the button is touch-activated.
	if (!Other || !HasSpawnFlags(SF_BUTTON_TOUCH_ACTIVATES))
	{
		return;
	}
	const EButtonCode Code = ButtonResponseToTouch();
	if (Code == EButtonCode::Nothing)
	{
		return;
	}
	if (bLocked)
	{
		PlaySoundField(UseLockedSound, true);
		return;
	}
	Activator = Other;
	Press(Other, Code);
}

ASourceFuncButton::EButtonCode ASourceFuncButton::ButtonResponseToTouch() const
{
	// CBaseButton::ButtonResponseToTouch
	if (ToggleState == ESourceToggleState::GoingUp ||
		ToggleState == ESourceToggleState::GoingDown ||
		(ToggleState == ESourceToggleState::AtTop && !bStayPushed && !HasSpawnFlags(SF_BUTTON_TOGGLE)))
	{
		return EButtonCode::Nothing;
	}

	if (ToggleState == ESourceToggleState::AtTop)
	{
		if (HasSpawnFlags(SF_BUTTON_TOGGLE) && !bStayPushed)
		{
			return EButtonCode::Return;
		}
	}
	else
	{
		return EButtonCode::Activate;
	}
	return EButtonCode::Nothing;
}

void ASourceFuncButton::Press(AActor* InActivator, EButtonCode Code)
{
	// CBaseButton::Press
	if (Code == EButtonCode::Press && (ToggleState == ESourceToggleState::GoingUp || ToggleState == ESourceToggleState::GoingDown))
	{
		return;
	}
	if (Code == EButtonCode::Activate && (ToggleState == ESourceToggleState::GoingUp || ToggleState == ESourceToggleState::AtTop))
	{
		return;
	}
	if (Code == EButtonCode::Return && (ToggleState == ESourceToggleState::GoingDown || ToggleState == ESourceToggleState::AtBottom))
	{
		return;
	}

	if (bLocked)
	{
		PlaySoundField(UseLockedSound, true);
		return;
	}

	// Temporarily disable the touch function, until movement is finished.
	bTouchEnabled = false;

	if ((Code == EButtonCode::Press && ToggleState == ESourceToggleState::AtTop) ||
		(Code == EButtonCode::Return && (ToggleState == ESourceToggleState::AtTop || ToggleState == ESourceToggleState::GoingUp)))
	{
		PlaySoundField(PressOutSound, false);
		FireOutput(TEXT("OnPressed"), InActivator);
		ButtonReturn();
	}
	else if (Code == EButtonCode::Press ||
		(Code == EButtonCode::Activate && (ToggleState == ESourceToggleState::AtBottom || ToggleState == ESourceToggleState::GoingDown)))
	{
		FireOutput(TEXT("OnPressed"), InActivator);
		ButtonActivate();
	}
}

bool ASourceFuncButton::OnUseLocked(AActor* InActivator)
{
	// CBaseButton::OnUseLocked
	PlaySoundField(UseLockedSound, true);
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	if (Now > UseLockedTime)
	{
		FireOutput(TEXT("OnUseLocked"), InActivator);
		UseLockedTime = Now + 0.5f;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------------------------------------------------

void ASourceFuncButton::ButtonActivate()
{
	// CBaseButton::ButtonActivate
	PlaySoundField(PressInSound, false);

	if (bLocked)
	{
		PlaySoundField(UseLockedSound, true);
		return;
	}

	ToggleState = ESourceToggleState::GoingUp;
	PendingMove = EPendingMove::TriggerAndWait;
	LinearMove(Position2, Speed);
}

void ASourceFuncButton::TriggerAndWait()
{
	// CBaseButton::TriggerAndWait
	if (bLocked)
	{
		return;
	}
	ToggleState = ESourceToggleState::AtTop;

	// Re-instate touches if the button is of the toggle variety.
	if (bStayPushed || HasSpawnFlags(SF_BUTTON_TOGGLE))
	{
		bTouchEnabled = HasSpawnFlags(SF_BUTTON_TOUCH_ACTIVATES);
		PendingMove = EPendingMove::None;
		MoveDoneTime = -1.0f;
	}
	else
	{
		// If button automatically comes back out, start it moving out after m_flWait.
		PendingMove = EPendingMove::Return;
		SetMoveDoneTime(Wait);
	}

	FireOutput(TEXT("OnIn"), Activator.Get());
}

void ASourceFuncButton::ButtonReturn()
{
	// CBaseButton::ButtonReturn
	ToggleState = ESourceToggleState::GoingDown;
	PendingMove = EPendingMove::BackHome;
	LinearMove(Position1, Speed);
}

void ASourceFuncButton::ButtonBackHome()
{
	// CBaseButton::ButtonBackHome
	ToggleState = ESourceToggleState::AtBottom;
	FireOutput(TEXT("OnOut"), Activator.Get());

	// Re-instate touch method, movement cycle is complete.
	bTouchEnabled = HasSpawnFlags(SF_BUTTON_TOUCH_ACTIVATES);
	PendingMove = EPendingMove::None;
	MoveDoneTime = -1.0f;
}

// ---------------------------------------------------------------------------------------------------------------------
// CBaseToggle::LinearMove
// ---------------------------------------------------------------------------------------------------------------------

void ASourceFuncButton::LinearMove(const FVector3f& DestPosition, float InSpeed)
{
	FinalDest = DestPosition;

	// Already there?
	if (DestPosition.Equals(SourceOrigin))
	{
		MoveDone();
		return;
	}

	// set destdelta to the vector needed to move
	const FVector3f DestDelta = DestPosition - SourceOrigin;

	// divide vector length by speed to get time to reach dest
	float TravelTime = DestDelta.Size() / InSpeed;
	const float MinTravelTime = 0.01f;
	if (TravelTime < MinTravelTime)
	{
		TravelTime = MinTravelTime;
	}
	SetMoveDoneTime(TravelTime);

	// scale the destdelta vector by the time spent traveling to get velocity
	Velocity = DestDelta * (1.0f / TravelTime);
}

void ASourceFuncButton::LinearMoveDone()
{
	// CBaseToggle::LinearMoveDone
	SetSourceOrigin(FinalDest);
	Velocity = FVector3f::ZeroVector;
	MoveDoneTime = -1.0f;
}

void ASourceFuncButton::MoveDone()
{
	const EPendingMove Pending = PendingMove;
	PendingMove = EPendingMove::None;

	switch (Pending)
	{
	case EPendingMove::TriggerAndWait:
		LinearMoveDone();
		TriggerAndWait();
		break;
	case EPendingMove::BackHome:
		LinearMoveDone();
		ButtonBackHome();
		break;
	case EPendingMove::Return:
		MoveDoneTime = -1.0f;
		ButtonReturn();
		break;
	default:
		MoveDoneTime = -1.0f;
		break;
	}
}

void ASourceFuncButton::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Touch links: a player's box overlapping ours counts as a touch.
	if (bTouchEnabled && HasSpawnFlags(SF_BUTTON_TOUCH_ACTIVATES))
	{
		const FBox Box = BrushMesh->Bounds.GetBox();
		for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
		{
			APawn* Pawn = It->IsValid() ? It->Get()->GetPawn() : nullptr;
			if (Pawn && Box.Intersect(Pawn->GetComponentsBoundingBox()))
			{
				ButtonTouch(Pawn);
				break;
			}
		}
	}

	if (MoveDoneTime < 0.0f)
	{
		return;
	}

	// MOVETYPE_PUSH: integrate velocity, then fire MoveDone when the move time elapses.
	if (!Velocity.IsNearlyZero())
	{
		const float Step = FMath::Min(DeltaSeconds, MoveDoneTime);
		if (Step > 0.0f)
		{
			SetSourceOrigin(SourceOrigin + Velocity * Step);
		}
	}

	MoveDoneTime -= DeltaSeconds;
	if (MoveDoneTime <= 0.0f)
	{
		MoveDone();
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Inputs and sounds
// ---------------------------------------------------------------------------------------------------------------------

bool ASourceFuncButton::AcceptInput(const FString& InputName, AActor* InActivator, AActor* Caller, const FString& Parameter)
{
	// CBaseButton's DEFINE_INPUTFUNC list.
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
	if (InputName.Equals(TEXT("Press"), ESearchCase::IgnoreCase))
	{
		Activator = InActivator;
		Press(InActivator, EButtonCode::Press);
		return true;
	}
	if (InputName.Equals(TEXT("PressIn"), ESearchCase::IgnoreCase))
	{
		Activator = InActivator;
		Press(InActivator, EButtonCode::Activate);
		return true;
	}
	if (InputName.Equals(TEXT("PressOut"), ESearchCase::IgnoreCase))
	{
		Activator = InActivator;
		Press(InActivator, EButtonCode::Return);
		return true;
	}
	if (InputName.Equals(TEXT("Use"), ESearchCase::IgnoreCase))
	{
		ButtonUse(InActivator);
		return true;
	}
	return Super::AcceptInput(InputName, InActivator, Caller, Parameter);
}

/** BUTTON_SOUNDWAIT: how long a retriggerable sound is held off before it may play again (buttons.cpp). */
static constexpr float BUTTON_SOUNDWAIT = 0.5f;

void ASourceFuncButton::Lock()
{
	bLocked = true;
	PlaySoundField(LockSound, false);
}

void ASourceFuncButton::Unlock()
{
	bLocked = false;
	PlaySoundField(UnlockSound, false);
}

void ASourceFuncButton::PlaySoundField(const FString& SoundName, bool bDebounce)
{
	// CreateWaveResolved takes either spelling: anything ending in .wav is a file under sound/, anything else
	// is a soundscript entry. That is what lets one keyvalue accept both a hand-picked wav and "Buttons.snd1".
	if (SoundName.IsEmpty() || SoundName == TEXT("0"))
	{
		return;
	}

	if (bDebounce)
	{
		// PlayLockSounds' BUTTON_SOUNDWAIT: a player holding use against a locked button would otherwise pile
		// the denial sound on top of itself several times a second.
		const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		if (Now <= LockSoundWaitTime)
		{
			return;
		}
		LockSoundWaitTime = Now + BUTTON_SOUNDWAIT;
	}

	float Volume = 1.0f, Pitch = 1.0f;
	if (ULambdaSoundWave* Wave = FLambdaSoundCache::Get().CreateWaveResolved(this, SoundName, false, Volume, Pitch))
	{
		UGameplayStatics::SpawnSoundAtLocation(this, Wave, GetActorLocation(), FRotator::ZeroRotator, Volume, Pitch);
	}
}
