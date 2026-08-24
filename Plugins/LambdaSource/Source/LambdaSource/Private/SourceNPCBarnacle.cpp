#include "SourceNPCBarnacle.h"
#include "Components/CapsuleComponent.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "SourceBSPFile.h"
#include "LambdaSourceModule.h"
#include "LambdaSourceSettings.h"
#include "SourceAmmoDef.h"
#include "SourceDamage.h"
#include "SourceImpactEffects.h"
#include "SourceCoordinates.h"
#include "SourceMDLFile.h"
#include "SourceStudioModelComponent.h"

namespace
{
	// npc_barnacle.h / npc_barnacle.cpp
	constexpr float BARNACLE_PULL_SPEED = 80.0f;			// units/s
	constexpr float BARNACLE_BITE_DAMAGE_TO_PLAYER = 15.0f;
	constexpr float BARNACLE_BITE_Z_OFFSET = 60.0f;			// height above the tip at which the bite lands
	constexpr float BARNACLE_PLAYER_EXTRA_LIFT = 25.0f;		// "additional height for the player to avoid view clipping"
	constexpr float BARNACLE_TONGUE_TRACE_UNITS = 2048.0f;	// TongueTouchEnt's trace
	constexpr float BARNACLE_CHECK_SPACING = 20.0f;			// how far off the tongue's column a victim may be
	constexpr float BARNACLE_DEAD_TONGUE_ALTITUDE = 32.0f;
	constexpr float BARNACLE_DIGEST_TIME = 10.0f;
	constexpr int32 BARNACLE_TONGUE_POINTS = 8;				// tongue1..tongue8
}

ASourceNPCBarnacle::ASourceNPCBarnacle(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void ASourceNPCBarnacle::Spawn()
{
	// CNPC_Barnacle::Spawn. The HL:A barnacle converted by Tools/ImportSource2Model.py is preferred; HL2's
	// model is the fallback.
	if (!SetModel(TEXT("models/hla/barnacle.mdl")))
	{
		SetModel(TEXT("models/barnacle.mdl"));
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;

	// UTIL_SetSize( this, Vector(-16, -16, -40), Vector(16, 16, 0) ): the barnacle's box hangs BELOW its origin.
	// The spawner placed us feet-at-origin, capsule above - a ceiling feeder is the other way up, so the capsule
	// is moved down to cover the hanging body and the model is raised so its root sits back at the origin.
	SetHull(16.0f, 40.0f);
	if (Model)
	{
		Model->SetRelativeLocation(FVector(0, 0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight()));
	}

	// MOVETYPE_NONE: the barnacle does not go anywhere.
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->SetMovementMode(MOVE_None);
	}

	BloodColor = ESourceBloodColor::Green;	// SetBloodColor( BLOOD_COLOR_GREEN )
	MaxHealth = Health = FSourceAmmoDef::Get().GetSkillValue(TEXT("sk_barnacle_health"), 35.0f);
	NPCState = ESourceNPCState::Idle;
	SetActivity(TEXT("ACT_IDLE"));

	// The tongue starts retracted and lowers at pull speed over the first seconds (DropTongue) - Spawn runs
	// before the entity is placed, so measuring the floor here would measure the wrong room.
	AltitudeUnits = 0.0f;

	UE_LOG(LogLambdaSource, Log, TEXT("npc_barnacle: health %.0f"), Health);
}

void ASourceNPCBarnacle::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UpdateTongue();

	// The victim hangs from the tongue: pinned under the barnacle and hauled upward. Done every frame, not every
	// think, or the ride is visibly steppy - this is the fixed constraint to the tongue tip, done with velocity.
	ACharacter* Held = Cast<ACharacter>(Victim.Get());
	if (Held && (Phase == EBarnaclePhase::Lifting || Phase == EBarnaclePhase::Biting))
	{
		const float Scale = ULambdaSourceSettings::Get().UnitScale;
		if (UCharacterMovementComponent* Move = Held->GetCharacterMovement())
		{
			const FVector At = Held->GetActorLocation();
			FVector Velocity;
			// Pulled back under the barnacle...
			Velocity.X = (TongueRootCm.X - At.X) * 4.0f;
			Velocity.Y = (TongueRootCm.Y - At.Y) * 4.0f;
			if (Phase == EBarnaclePhase::Lifting)
			{
				// ...and up, throbbing on the sine the way PullEnemyTorwardsMouth pulls the tongue in.
				Velocity.Z = PullSpeedUnits * Scale * FMath::Abs(FMath::Sin(LocalTimer * 5.0f));
			}
			else
			{
				// At the mouth: held at the bite height.
				const float BiteZ = BARNACLE_BITE_Z_OFFSET + (Held->IsPlayerControlled() ? BARNACLE_PLAYER_EXTRA_LIFT : 0.0f);
				const float WantedZ = TongueRootCm.Z - BiteZ * Scale - Held->BaseEyeHeight;
				Velocity.Z = FMath::Clamp((WantedZ - At.Z) * 3.0f, -100.0f * Scale, 100.0f * Scale);
			}
			Move->Velocity = Velocity;
		}
	}
}

void ASourceNPCBarnacle::NPCThink()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const float Now = World->GetTimeSeconds();

	// The spawner placed us feet-at-origin AFTER Spawn() ran - and worse, the character movement comes back up in
	// Walking and the barnacle falls off the ceiling before the first think. Mount it here, explicitly, from the
	// entity's own origin: capsule hanging below the mount point, model root at it, and no movement ever again.
	if (!bMounted)
	{
		bMounted = true;
		const float Scale = ULambdaSourceSettings::Get().UnitScale;
		FVector3f OriginUnits = FVector3f::ZeroVector;
		if (Entity.GetVector(TEXT("origin"), OriginUnits))
		{
			const FVector MountCm = FSourceCoords::ToUE(OriginUnits, Scale);
			const float HalfHeightCm = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			SetActorLocation(MountCm - FVector(0, 0, HalfHeightCm), false, nullptr, ETeleportType::TeleportPhysics);
		}
		if (UCharacterMovementComponent* Move = GetCharacterMovement())
		{
			Move->DisableMovement();
		}
		UE_LOG(LogLambdaSource, Log, TEXT("npc_barnacle mounted: capsule centre %s half-height %.0f, collision %d"),
			*(FSourceCoords::ToSource(GetActorLocation(), ULambdaSourceSettings::Get().UnitScale)).ToCompactString(),
			GetCapsuleComponent()->GetScaledCapsuleHalfHeight() / ULambdaSourceSettings::Get().UnitScale,
			(int32)GetCapsuleComponent()->GetCollisionResponseToChannel(ECC_Visibility));
	}

	if (Phase == EBarnaclePhase::Dead)
	{
		// WaitTillDead: the tongue draws up to its dead length, and the death animation settles into the
		// dead idle.
		AltitudeUnits = FMath::Max(BARNACLE_DEAD_TONGUE_ALTITUDE, AltitudeUnits - PullSpeedUnits * ThinkInterval);
		if (IsActivityFinished() && GetActivity() == TEXT("ACT_DIESIMPLE"))
		{
			SetActivity(TEXT("ACT_DIE_IDLE"));
		}
		return;
	}

	// Whatever phase we're in, a vanished victim means the meal is over.
	if (Phase != EBarnaclePhase::Idle && Phase != EBarnaclePhase::Digesting && !Victim.IsValid())
	{
		LostPrey();
	}

	switch (Phase)
	{
	case EBarnaclePhase::Idle:
	{
		// "this is done so barnacle will fidget."
		if (IsActivityFinished() && GetActivity() != TEXT("ACT_IDLE"))
		{
			SetActivity(TEXT("ACT_IDLE"));
		}

		AActor* Touch = nullptr;
		const float RestLength = TongueTouchLength(Touch);

		// "If there's something under us, lower the tongue down so we can grab it."
		if (AltitudeUnits < RestLength)
		{
			AltitudeUnits = FMath::Min(RestLength, AltitudeUnits + PullSpeedUnits * ThinkInterval);
		}
		else
		{
			AltitudeUnits = RestLength;
			if (Touch)
			{
				AttachTongue(Touch);
			}
		}
		break;
	}

	case EBarnaclePhase::Lifting:
		LiftPrey();
		break;

	case EBarnaclePhase::Biting:
		if (Now >= NextBiteTime && NextBiteTime > 0.0f)
		{
			NextBiteTime = 0.0f;
			BitePrey();
		}
		else if (NextBiteTime == 0.0f && IsActivityFinished())
		{
			// The victim survived the bite: bite again.
			if (Victim.IsValid())
			{
				SetActivity(TEXT("ACT_BARNACLE_BITE_PLAYER"));
				NextBiteTime = Now + 1.0f;
			}
		}
		break;

	case EBarnaclePhase::Digesting:
		// "bite prey every once in a while"
		if (FMath::RandRange(0, 25) == 0)
		{
			EmitSound(TEXT("NPC_Barnacle.Digest"));
		}
		if (IsActivityFinished())
		{
			SetActivity(TEXT("ACT_BARNACLE_CHEW_SMALL_THINGS"));
		}
		if (Now >= DigestFinishTime)
		{
			Phase = EBarnaclePhase::Idle;
			SetActivity(TEXT("ACT_IDLE"));
		}
		break;

	default:
		break;
	}
}

void ASourceNPCBarnacle::UpdateTongue()
{
	if (!Model || !Model->HasModel())
	{
		return;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;

	// TongueTouchEnt traces from GetAbsOrigin() - the model's mount point at the ceiling. The converted model's
	// TongueRoot attachment sits above the origin, inside the ceiling brush, and is no use as a trace start.
	TongueRootCm = Model->GetComponentLocation();
	UE_LOG(LogLambdaSource, VeryVerbose, TEXT("barnacle tongue: root %s, altitude %.0f"),
		*(FSourceCoords::ToSource(TongueRootCm, Scale)).ToCompactString(), AltitudeUnits);
	TongueTipCm = TongueRootCm - FVector(0, 0, AltitudeUnits * Scale);

	// C_NPC_Barnacle::BuildTransformations: the tongue bones are spaced evenly between the root and the tip;
	// the animation keeps their wiggle, the code owns how far down they reach.
	const FTransform ComponentToWorld = Model->GetComponentTransform();
	for (int32 i = 0; i < BARNACLE_TONGUE_POINTS; ++i)
	{
		const float Fraction = (float)i / (float)(BARNACLE_TONGUE_POINTS - 1);
		const FVector World = FMath::Lerp(TongueRootCm, TongueTipCm, Fraction);
		Model->SetBonePositionOverride(FString::Printf(TEXT("tongue%d"), i + 1),
			ComponentToWorld.InverseTransformPosition(World));
	}
}

float ASourceNPCBarnacle::TongueTouchLength(AActor*& OutTouch) const
{
	OutTouch = nullptr;
	UWorld* World = GetWorld();
	if (!World)
	{
		return AltitudeUnits;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;

	// "trace once to hit architecture and see if the tongue needs to change position" - brush only, then
	// pull it up a tad.
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(LambdaBarnacleTongue), false, this);
	float Length = BARNACLE_TONGUE_TRACE_UNITS;
	if (World->LineTraceSingleByObjectType(Hit, TongueRootCm,
		TongueRootCm - FVector(0, 0, BARNACLE_TONGUE_TRACE_UNITS * Scale),
		FCollisionObjectQueryParams(ECC_WorldStatic), Params))
	{
		Length = (TongueRootCm.Z - Hit.ImpactPoint.Z) / Scale;
	}
	Length = FMath::Max(8.0f, Length - RestUnitsAboveGround);
	UE_LOG(LogLambdaSource, VeryVerbose, TEXT("barnacle trace: root %s -> %s, length %.0f"),
		*(FSourceCoords::ToSource(TongueRootCm, Scale)).ToCompactString(),
		Hit.bBlockingHit ? *(FSourceCoords::ToSource(Hit.ImpactPoint, Scale)).ToCompactString() : TEXT("no hit"), Length);

	// TongueTouchEnt: anything standing in the tongue's column, top of them above the tip.
	for (TActorIterator<ACharacter> It(World); It; ++It)
	{
		ACharacter* Candidate = *It;
		if (Candidate == this)
		{
			continue;
		}
		if (ASourceNPCBase* NPC = Cast<ASourceNPCBase>(Candidate))
		{
			if (!NPC->IsAlive() || Cast<ASourceNPCBarnacle>(NPC))
			{
				continue;
			}
		}
		const FVector At = Candidate->GetActorLocation();
		const float Dist2D = FVector::Dist2D(At, TongueRootCm) / Scale;
		if (Dist2D > BARNACLE_CHECK_SPACING)
		{
			continue;
		}
		const float TopZ = At.Z + Candidate->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		if (TopZ >= TongueRootCm.Z - AltitudeUnits * Scale && TopZ < TongueRootCm.Z)
		{
			OutTouch = Candidate;
			break;
		}
	}
	return Length;
}

void ASourceNPCBarnacle::AttachTongue(AActor* NewVictim)
{
	// AttachTongueToTarget.
	EmitSound(TEXT("NPC_Barnacle.BreakNeck"));
	EmitSound(FMath::RandBool() ? TEXT("NPC_Barnacle.PullPant") : TEXT("NPC_Barnacle.TongueStretch"));
	SetActivity(TEXT("ACT_BARNACLE_SLURP"));

	Victim = NewVictim;
	Phase = EBarnaclePhase::Lifting;
	// "Set the local timer to 60 seconds, which starts the lifting phase on the upshot of the sine wave."
	LocalTimer = 60.0f;
	bScreamed = false;
	SetVictimHeld(NewVictim, true);

	UE_LOG(LogLambdaSource, Log, TEXT("npc_barnacle grabbed %s"), *GetNameSafe(NewVictim));
}

void ASourceNPCBarnacle::LiftPrey()
{
	AActor* Prey = Victim.Get();
	if (!Prey)
	{
		LostPrey();
		return;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	ACharacter* PreyCharacter = Cast<ACharacter>(Prey);
	const bool bPlayer = PreyCharacter && PreyCharacter->IsPlayerControlled();

	// An NPC that died on the way up is dropped (the ragdoll lift is not ported).
	if (ASourceNPCBase* NPC = Cast<ASourceNPCBase>(Prey))
	{
		if (!NPC->IsAlive())
		{
			LostPrey();
			return;
		}
	}

	// PullEnemyTorwardsMouth: the sine-throbbed pull. The victim itself is moved in Tick.
	LocalTimer += ThinkInterval;
	const float Pull = FMath::Abs(FMath::Sin(LocalTimer * 5.0f)) * PullSpeedUnits * ThinkInterval;
	AltitudeUnits = FMath::Max(0.0f, AltitudeUnits - Pull);

	const float BiteZ = BARNACLE_BITE_Z_OFFSET + (bPlayer ? BARNACLE_PLAYER_EXTRA_LIFT : 0.0f);
	const float PreyEyeZ = Prey->GetActorLocation().Z + (PreyCharacter ? PreyCharacter->BaseEyeHeight : 0.0f);

	// PlayLiftingScream: "Play a scream when we're almost within bite range."
	if (!bScreamed && TongueRootCm.Z - PreyEyeZ < (BiteZ + 120.0f) * Scale)
	{
		bScreamed = true;
		EmitSound(TEXT("NPC_Barnacle.Scream"));
	}

	if (TongueRootCm.Z - PreyEyeZ < BiteZ * Scale)
	{
		// "Start the bite animation. The anim event in it will finish the job."
		Phase = EBarnaclePhase::Biting;
		SetActivity(bPlayer ? TEXT("ACT_BARNACLE_BITE_PLAYER") : TEXT("ACT_BARNACLE_BITE_HUMAN"));
		NextBiteTime = GetWorld()->GetTimeSeconds() + 1.0f;
	}
}

void ASourceNPCBarnacle::BitePrey()
{
	AActor* Prey = Victim.Get();
	if (!Prey)
	{
		LostPrey();
		return;
	}
	EmitSound(TEXT("NPC_Barnacle.FinalBite"));

	ACharacter* PreyCharacter = Cast<ACharacter>(Prey);
	const bool bPlayer = PreyCharacter && PreyCharacter->IsPlayerControlled();

	// "Kill the victim instantly" - except the player, who is chewed BARNACLE_BITE_DAMAGE_TO_PLAYER at a time.
	FHitResult BiteHit(Prey, nullptr, Prey->GetActorLocation(), FVector::UpVector);
	const float Damage = bPlayer ? BARNACLE_BITE_DAMAGE_TO_PLAYER : 10000.0f;
	FSourceDamageEvent Info(Damage, BiteHit, FVector(0, 0, -1), nullptr, FVector::ZeroVector,
		SourceDamageType::DMG_SLASH, SourceHitGroup::HITGROUP_HEAD);
	Prey->TakeDamage(Damage, Info, nullptr, this);

	if (bPlayer)
	{
		// Keep chewing: the think re-arms the bite when this one's animation has run out.
		return;
	}

	// SwallowPrey, condensed: the NPC goes down the gullet whole and the digestion begins.
	SourceImpact::SpawnBlood(GetWorld(), MaterialLibrary, TongueTipCm, FVector(0, 0, -1), BloodColor);
	SetVictimHeld(Prey, false);
	if (Victim.IsValid())
	{
		Victim->Destroy();
	}
	Victim.Reset();
	Phase = EBarnaclePhase::Digesting;
	DigestFinishTime = GetWorld()->GetTimeSeconds() + BARNACLE_DIGEST_TIME;
	SetActivity(TEXT("ACT_BARNACLE_CHEW_HUMAN"));
	AltitudeUnits = 0.0f;
}

void ASourceNPCBarnacle::LostPrey()
{
	// LostPrey.
	if (AActor* Prey = Victim.Get())
	{
		SetVictimHeld(Prey, false);
	}
	Victim.Reset();
	LocalTimer = 0.0f;
	NextBiteTime = 0.0f;
	if (Phase != EBarnaclePhase::Dead)
	{
		Phase = EBarnaclePhase::Idle;
		SetActivity(TEXT("ACT_IDLE"));
	}
}

void ASourceNPCBarnacle::SetVictimHeld(AActor* HeldVictim, bool bHeld)
{
	ACharacter* C = Cast<ACharacter>(HeldVictim);
	if (!C)
	{
		return;
	}
	UCharacterMovementComponent* Move = C->GetCharacterMovement();
	if (!Move)
	{
		return;
	}
	if (bHeld)
	{
		// The tongue owns the victim: no gravity, no ground - flying, driven by our velocity each frame.
		VictimSavedGravityScale = Move->GravityScale;
		Move->SetMovementMode(MOVE_Flying);
		Move->GravityScale = 0.0f;
		Move->Velocity = FVector::ZeroVector;
		if (ASourceNPCBase* NPC = Cast<ASourceNPCBase>(C))
		{
			NPC->StopMoving();
		}
	}
	else
	{
		Move->GravityScale = VictimSavedGravityScale > 0.0f ? VictimSavedGravityScale : 1.0f;
		Move->SetMovementMode(MOVE_Falling);
	}
}

void ASourceNPCBarnacle::Event_Killed(AActor* Attacker)
{
	// CNPC_Barnacle::Event_Killed: drop whatever is on the tongue, scream, die where it hangs - the barnacle's
	// death is its animation, not a ragdoll. (Death gibs are not ported.)
	Phase = EBarnaclePhase::Dead;
	NPCState = ESourceNPCState::Dead;
	LostPrey();

	EmitSound(TEXT("NPC_Barnacle.Die"));
	SetActivity(TEXT("ACT_DIESIMPLE"));

	// "Puke blood" at the mouth and onto the floor below.
	SourceImpact::SpawnBlood(GetWorld(), MaterialLibrary, GetActorLocation(), FVector(0, 0, -1), BloodColor);

	UE_LOG(LogLambdaSource, Log, TEXT("npc_barnacle killed by %s"), *GetNameSafe(Attacker));
}
