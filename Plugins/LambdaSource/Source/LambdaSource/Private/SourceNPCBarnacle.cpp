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
#include "SourcePropPhysics.h"
#include "SourceRagdoll.h"
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
	constexpr float BARNACLE_TONGUE_MAX_LIFT_MASS = 70.0f;	// npc_barnacle.h: heavier things stay on the floor
	/**
	 * A corpse does not travel at the speed it is told to: the velocity is applied to every piece and the joint
	 * solver averages most of it away. Measured over a lift, a commanded 51 units/s moved the ragdoll at 16, so
	 * the drive is scaled to put the haul back at the speed BARNACLE_PULL_SPEED asks for.
	 */
	constexpr float BARNACLE_RAGDOLL_DRIVE_SCALE = 3.2f;
}

ASourceNPCBarnacle::ASourceNPCBarnacle(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void ASourceNPCBarnacle::Spawn()
{
	// CNPC_Barnacle::Spawn. The HL:A barnacle converted by Tools/ImportSource2Model.py is preferred; HL2's
	// model is the fallback.
	if (!SetModel(TEXT("models/creatures/npc_barnacle/npc_barnacle.mdl")))
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

	if (Phase != EBarnaclePhase::Lifting && Phase != EBarnaclePhase::Biting && Phase != EBarnaclePhase::Swallowing)
	{
		return;
	}
	AActor* Prey = Victim.Get();
	if (!Prey)
	{
		return;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const FVector Held = GetHeldPoint();

	// The victim is pinned under the barnacle and hauled up. Done every frame, not every think, or the ride is
	// visibly steppy - this is the fixed constraint to the tongue tip, done with velocity.
	FVector Velocity;
	Velocity.X = (TongueRootCm.X - Held.X) * 4.0f;
	Velocity.Y = (TongueRootCm.Y - Held.Y) * 4.0f;
	if (Phase == EBarnaclePhase::Lifting)
	{
		// ...throbbing on the sine the way PullEnemyTorwardsMouth pulls the tongue in.
		Velocity.Z = PullSpeedUnits * Scale * FMath::Abs(FMath::Sin(LocalTimer * 5.0f));
	}
	else if (Phase == EBarnaclePhase::Swallowing)
	{
		// The last stretch into the mouth, at a steady crawl.
		Velocity.Z = 32.0f * Scale;
	}
	else
	{
		// At the mouth: held at the bite height.
		const ACharacter* PreyCharacter = Cast<ACharacter>(Prey);
		const float BiteZ = BARNACLE_BITE_Z_OFFSET
			+ (PreyCharacter && PreyCharacter->IsPlayerControlled() ? BARNACLE_PLAYER_EXTRA_LIFT : 0.0f);
		Velocity.Z = FMath::Clamp((TongueRootCm.Z - BiteZ * Scale - Held.Z) * 3.0f, -100.0f * Scale, 100.0f * Scale);
	}

	// ...whatever kind of thing it turned out to be.
	if (ASourceRagdoll* Corpse = VictimRagdoll.Get())
	{
		Corpse->SetHangVelocity(Velocity * BARNACLE_RAGDOLL_DRIVE_SCALE);
	}
	else if (ASourcePropPhysics* Prop = Cast<ASourcePropPhysics>(Prey))
	{
		if (UPrimitiveComponent* Body = Prop->GetPhysicsBody())
		{
			Body->SetPhysicsLinearVelocity(Velocity);
		}
	}
	else if (ACharacter* HeldCharacter = Cast<ACharacter>(Prey))
	{
		if (UCharacterMovementComponent* Move = HeldCharacter->GetCharacterMovement())
		{
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

	if (LastSpitEnemy.IsValid() && Now >= ForgetSpitTime)
	{
		LastSpitEnemy.Reset();
	}

	// Whatever phase we're in, a vanished victim means the meal is over.
	if (Phase != EBarnaclePhase::Idle && Phase != EBarnaclePhase::Digesting
		&& Phase != EBarnaclePhase::Swallowing && !Victim.IsValid())
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

	case EBarnaclePhase::Swallowing:
		SwallowPrey();
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

	// While something is on the tongue, the tongue's length IS wherever that thing is: it stays attached to what
	// it grabbed instead of keeping a length of its own (Source sets m_flAltitude from the victim's position).
	if (Phase == EBarnaclePhase::Lifting || Phase == EBarnaclePhase::Biting || Phase == EBarnaclePhase::Swallowing)
	{
		AltitudeUnits = FMath::Max(0.0f, (TongueRootCm.Z - GetHeldPoint().Z) / Scale);
	}
	TongueTipCm = TongueRootCm - FVector(0.0f, 0.0f, AltitudeUnits * Scale);

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

FVector ASourceNPCBarnacle::GetHeldPoint() const
{
	if (ASourceRagdoll* Corpse = VictimRagdoll.Get())
	{
		return Corpse->GetCentreOfMass();
	}
	if (AActor* Prey = Victim.Get())
	{
		// A living victim hangs by the head: Source grabs at EyePosition().
		if (const ACharacter* PreyCharacter = Cast<ACharacter>(Prey))
		{
			return Prey->GetActorLocation() + FVector(0.0f, 0.0f, PreyCharacter->BaseEyeHeight);
		}
		return Prey->GetActorLocation();
	}
	return TongueTipCm;
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
	// pull it up a tad. This is how far the tongue hangs with nothing under it.
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

	// TongueTouchEnt: whatever is standing in the tongue's column. When it finds something the length becomes
	// the distance to THAT, not to the floor - "allow the barnacles to grab stuff while their tongue is
	// lowering" - so the tongue reaches exactly what it is about to take hold of.
	auto ReachTo = [&](float TargetZ)
	{
		return FMath::Max(8.0f, (TongueRootCm.Z - TargetZ) / Scale - RestUnitsAboveGround);
	};

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
		if (FVector::Dist2D(At, TongueRootCm) / Scale > BARNACLE_CHECK_SPACING)
		{
			continue;
		}
		const float TopZ = At.Z + Candidate->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		if (TopZ >= TongueRootCm.Z - AltitudeUnits * Scale && TopZ < TongueRootCm.Z)
		{
			OutTouch = Candidate;
			// Grabbed by the head, as Source grabs at EyePosition().
			return ReachTo(At.Z + Candidate->BaseEyeHeight);
		}
	}

	// "Deal with physics objects": anything light enough for the tongue to lift - including one the player is
	// holding, which the tongue simply takes off him.
	for (TActorIterator<ASourcePropPhysics> It(World); It; ++It)
	{
		ASourcePropPhysics* Prop = *It;
		if (Prop == LastSpitEnemy.Get() || Prop->GetMass() > BARNACLE_TONGUE_MAX_LIFT_MASS)
		{
			continue;
		}
		const FVector At = Prop->GetActorLocation();
		if (FVector::Dist2D(At, TongueRootCm) / Scale > BARNACLE_CHECK_SPACING)
		{
			continue;
		}
		// HL2 lowers the tongue to a physics object rather than waiting for contact, so a low crate under the
		// resting tip is still taken.
		const float TopZ = At.Z + Prop->GetHullExtent().Z;
		const float TipZ = TongueRootCm.Z - AltitudeUnits * Scale;
		if (TopZ + RestUnitsAboveGround * Scale >= TipZ && TopZ < TongueRootCm.Z)
		{
			OutTouch = Prop;
			return ReachTo(At.Z);
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
	VictimRagdoll = nullptr;
	Phase = EBarnaclePhase::Lifting;
	// "Set the local timer to 60 seconds, which starts the lifting phase on the upshot of the sine wave."
	LocalTimer = 60.0f;
	bScreamed = false;

	// A prop in the player's hands is pulled out of them.
	if (ASourcePropPhysics* Prop = Cast<ASourcePropPhysics>(NewVictim))
	{
		if (Prop->IsHeld())
		{
			Prop->RevokeCarry();
		}
	}

	// AttachRagdollToTongue: an NPC is handed to physics the moment it is caught - no death sound, no death
	// animation, it just goes limp on the tongue - and the corpse is what the barnacle carries from here.
	if (ASourceNPCBase* NPC = Cast<ASourceNPCBase>(NewVictim))
	{
		if (ASourceRagdoll* Corpse = NPC->BecomeRagdollSilent())
		{
			VictimRagdoll = Corpse;
			VictimBody = NPC;
			Victim = Corpse;
			Corpse->SetHeldByTongue(true);
			// The NPC actor stays: our ragdoll owns no mesh of its own, it poses the NPC's model component, so
			// the corpse would vanish with it. It is dead, its hull is off, and the ragdoll drives it from here.
		}
	}
	SetVictimHeld(Victim.Get(), true);

	UE_LOG(LogLambdaSource, Log, TEXT("npc_barnacle grabbed %s%s"), *GetNameSafe(NewVictim),
		VictimRagdoll.IsValid() ? TEXT(" (ragdolled onto the tongue)") : TEXT(""));
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

	// A living NPC that died on the way up is dropped (a ragdoll on the tongue is fine - that is the meal).
	if (ASourceNPCBase* NPC = Cast<ASourceNPCBase>(Prey))
	{
		if (!NPC->IsAlive())
		{
			LostPrey();
			return;
		}
	}

	// PullEnemyTorwardsMouth: the pull throbs on a sine. The victim is moved in Tick and the tongue's length
	// follows it, so there is no altitude to wind down by hand here.
	LocalTimer += ThinkInterval;

	const float BiteZ = BARNACLE_BITE_Z_OFFSET + (bPlayer ? BARNACLE_PLAYER_EXTRA_LIFT : 0.0f);
	const float HeldZ = GetHeldPoint().Z;
	UE_LOG(LogLambdaSource, Verbose, TEXT("barnacle lifting %s: held %.0f units below the mouth (bite at %.0f), altitude %.0f"),
		*GetNameSafe(Prey), (TongueRootCm.Z - HeldZ) / Scale, BiteZ, AltitudeUnits);

	// PlayLiftingScream: "Play a scream when we're almost within bite range."
	if (!bScreamed && TongueRootCm.Z - HeldZ < (BiteZ + 120.0f) * Scale)
	{
		bScreamed = true;
		EmitSound(TEXT("NPC_Barnacle.Scream"));
	}

	if (TongueRootCm.Z - HeldZ < BiteZ * Scale)
	{
		// "Start the bite animation. The anim event in it will finish the job."
		Phase = EBarnaclePhase::Biting;
		SetActivity(bPlayer ? TEXT("ACT_BARNACLE_BITE_PLAYER")
			: Cast<ASourcePropPhysics>(Prey) ? TEXT("ACT_BARNACLE_TASTE_SPIT") : TEXT("ACT_BARNACLE_BITE_HUMAN"));
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
	// "Yuck! Me no like that!" - a crate is no meal: it is tasted and flung away (SpitPrey).
	if (Cast<ASourcePropPhysics>(Prey))
	{
		SpitPrey();
		return;
	}
	EmitSound(TEXT("NPC_Barnacle.FinalBite"));

	ACharacter* PreyCharacter = Cast<ACharacter>(Prey);
	const bool bPlayer = PreyCharacter && PreyCharacter->IsPlayerControlled();

	if (bPlayer)
	{
		// The player is chewed BARNACLE_BITE_DAMAGE_TO_PLAYER at a time; the think re-arms the bite.
		FHitResult BiteHit(Prey, nullptr, Prey->GetActorLocation(), FVector::UpVector);
		FSourceDamageEvent Info(BARNACLE_BITE_DAMAGE_TO_PLAYER, BiteHit, FVector(0, 0, -1), nullptr,
			FVector::ZeroVector, SourceDamageType::DMG_SLASH, SourceHitGroup::HITGROUP_HEAD);
		Prey->TakeDamage(BARNACLE_BITE_DAMAGE_TO_PLAYER, Info, nullptr, this);
		return;
	}

	// SwallowPrey: the corpse is drawn up into the mouth. Blood at the bite hides the join, as HL2 sprays to
	// cover the headcrab it bites off a zombie.
	SourceImpact::SpawnBloodSpray(GetWorld(), MaterialLibrary, GetHeldPoint(), FVector(0, 0, -1),
		VictimRagdoll.IsValid() ? VictimRagdoll->GetBloodColor() : BloodColor, 8.0f);

	if (VictimRagdoll.IsValid())
	{
		Phase = EBarnaclePhase::Swallowing;
		SetActivity(TEXT("ACT_BARNACLE_CHEW_HUMAN"));
		return;
	}

	// Nothing to swallow (no collision model for a corpse): straight to digesting.
	SetVictimHeld(Prey, false);
	Prey->Destroy();
	Victim.Reset();
	Phase = EBarnaclePhase::Digesting;
	DigestFinishTime = GetWorld()->GetTimeSeconds() + BARNACLE_DIGEST_TIME;
	SetActivity(TEXT("ACT_BARNACLE_CHEW_HUMAN"));
	AltitudeUnits = 0.0f;
}

void ASourceNPCBarnacle::SwallowPrey()
{
	// "Slowly swallow the prey whole" - the corpse travels up the last stretch and disappears into the mouth,
	// and the barnacle spits blood while it works (SprayBlood).
	ASourceRagdoll* Corpse = VictimRagdoll.Get();
	UWorld* World = GetWorld();
	if (!Corpse || !World)
	{
		Phase = EBarnaclePhase::Digesting;
		DigestFinishTime = World ? World->GetTimeSeconds() + BARNACLE_DIGEST_TIME : 0.0f;
		Victim.Reset();
		return;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;

	if (FMath::RandRange(0, 3) == 0)
	{
		EmitSound(TEXT("NPC_Barnacle.Digest"));
		SourceImpact::SpawnBloodSpray(World, MaterialLibrary, GetHeldPoint(), FVector(0, 0, -1),
			Corpse->GetBloodColor(), 4.0f);
	}

	// Fully swallowed once its middle reaches the mouth.
	if ((TongueRootCm.Z - Corpse->GetCentreOfMass().Z) / Scale <= 8.0f)
	{
		SourceImpact::SpawnBloodSpray(World, MaterialLibrary, TongueRootCm, FVector(0, 0, -1),
			Corpse->GetBloodColor(), 8.0f);
		Corpse->Destroy();
		if (AActor* Body = VictimBody.Get())
		{
			Body->Destroy();
		}
		VictimRagdoll = nullptr;
		VictimBody = nullptr;
		Victim.Reset();
		Phase = EBarnaclePhase::Digesting;
		DigestFinishTime = World->GetTimeSeconds() + BARNACLE_DIGEST_TIME;
		AltitudeUnits = 0.0f;
		UE_LOG(LogLambdaSource, Log, TEXT("npc_barnacle swallowed its meal"));
	}
}


void ASourceNPCBarnacle::SpitPrey()
{
	AActor* Prey = Victim.Get();
	if (!Prey)
	{
		return;
	}
	EmitSound(TEXT("NPC_Barnacle.Spit"));
	// "Spit out the prey; add physics force!" - flung sideways and down, and remembered so the tongue does not
	// scoop the same crate straight back up.
	if (ASourcePropPhysics* Prop = Cast<ASourcePropPhysics>(Prey))
	{
		if (UPrimitiveComponent* Body = Prop->GetPhysicsBody())
		{
			const float Scale = ULambdaSourceSettings::Get().UnitScale;
			const FVector Fling = FVector(FMath::FRandRange(-1.0f, 1.0f), FMath::FRandRange(-1.0f, 1.0f), -0.3f).GetSafeNormal();
			Body->SetPhysicsLinearVelocity(Fling * 350.0f * Scale);
			Body->SetPhysicsAngularVelocityInDegrees(FVector(FMath::FRandRange(-360.0f, 360.0f),
				FMath::FRandRange(-360.0f, 360.0f), FMath::FRandRange(-360.0f, 360.0f)));
		}
	}
	LastSpitEnemy = Prey;
	ForgetSpitTime = GetWorld()->GetTimeSeconds() + 8.0f;
	Victim.Reset();
	Phase = EBarnaclePhase::Idle;
	SetActivity(TEXT("ACT_BARNACLE_TASTE_SPIT"));
	AltitudeUnits = 0.0f;
}

void ASourceNPCBarnacle::SpawnDeathGibs()
{
	// CNPC_Barnacle::SpawnDeathGibs: each gib in the list has a coin-flip chance of coming up, and at least one
	// always does. They are physics props tumbling out of the mouth, gone after their fade time.
	static const TCHAR* GibNames[] = {
		TEXT("models/gibs/hgibs.mdl"),
		TEXT("models/gibs/hgibs_scapula.mdl"),
		TEXT("models/gibs/hgibs_rib.mdl"),
		TEXT("models/gibs/hgibs_spine.mdl"),
	};
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	bool bDroppedAny = false;
	for (int32 i = 0; i < UE_ARRAY_COUNT(GibNames); ++i)
	{
		const bool bLast = i == UE_ARRAY_COUNT(GibNames) - 1;
		if (!FMath::RandBool() && !(bLast && !bDroppedAny))
		{
			continue;
		}
		bDroppedAny = true;
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ASourcePropPhysics* Gib = World->SpawnActor<ASourcePropPhysics>(ASourcePropPhysics::StaticClass(), FTransform::Identity, Params);
		if (!Gib)
		{
			continue;
		}
		// SpawnSpecificGibs( this, 1, 32, 1, ... ): out of the mouth at a gentle 32 units/s, scattered.
		const FVector Velocity = FVector(FMath::FRandRange(-1.0f, 1.0f), FMath::FRandRange(-1.0f, 1.0f), -1.0f).GetSafeNormal() * 32.0f * Scale;
		Gib->InitializeAsGib(GibNames[i], MaterialLibrary,
			FTransform(FRotator(0.0f, FMath::FRandRange(0.0f, 360.0f), 0.0f), GetActorLocation()),
			Velocity, FVector(FMath::FRandRange(-180.0f, 180.0f), FMath::FRandRange(-180.0f, 180.0f), FMath::FRandRange(-180.0f, 180.0f)),
			20.0f);
	}
}

void ASourceNPCBarnacle::LostPrey()
{
	// LostPrey.
	if (AActor* Prey = Victim.Get())
	{
		SetVictimHeld(Prey, false);
	}
	// A corpse on the tongue is simply dropped - gravity back on, and it falls where it hung.
	if (ASourceRagdoll* Corpse = VictimRagdoll.Get())
	{
		Corpse->SetHeldByTongue(false);
	}
	VictimRagdoll = nullptr;
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
	// A speared physics prop keeps simulating; the velocity drive in Tick is all the holding it needs.
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

	// "Puke blood": UTIL_BloodSpray( GetAbsOrigin(), Vector(0,0,-1), BLOOD_COLOR_RED, 8, FX_BLOODSPRAY_ALL ) -
	// a proper spray downward out of the mouth, red as HL2 spits it whatever colour the barnacle bleeds, and
	// the gibs come up with it.
	SourceImpact::SpawnBloodSpray(GetWorld(), MaterialLibrary, TongueRootCm, FVector(0, 0, -1),
		ESourceBloodColor::Red, 8.0f);
	// Put blood on the ground if near enough (UTIL_BloodDecalTrace).
	if (UWorld* World = GetWorld())
	{
		const float Scale = ULambdaSourceSettings::Get().UnitScale;
		FHitResult Ground;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(LambdaBarnacleBlood), false, this);
		if (World->LineTraceSingleByObjectType(Ground, TongueRootCm, TongueRootCm - FVector(0, 0, 256.0f * Scale),
			FCollisionObjectQueryParams(ECC_WorldStatic), Params))
		{
			SourceImpact::SpawnDecal(Ground, MaterialLibrary, TEXT("Blood"));
		}
	}
	SpawnDeathGibs();

	UE_LOG(LogLambdaSource, Log, TEXT("npc_barnacle killed by %s"), *GetNameSafe(Attacker));
}
