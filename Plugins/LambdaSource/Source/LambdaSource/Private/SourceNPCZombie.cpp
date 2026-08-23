#include "SourceNPCZombie.h"
#include "LambdaSourceModule.h"
#include "LambdaSourceSettings.h"
#include "SourceAmmoDef.h"
#include "SourceBSPWorldActor.h"
#include "SourceCoordinates.h"
#include "SourceDamage.h"
#include "SourceImpactEffects.h"
#include "SourcePlayerPunch.h"
#include "SourceStudioModelComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"

using namespace SourceDamageType;
using namespace SourceHitGroup;

namespace
{
	constexpr float ZOMBIE_MELEE_REACH = 55.0f;				// npc_BaseZombie.h
	constexpr float ZOMBIE_BULLET_DAMAGE_SCALE = 0.5f;		// npc_BaseZombie.cpp
	constexpr float ZOMBIE_BUCKSHOT_TRIPLE_DAMAGE_DIST = 96.0f;
	constexpr int32 ZOMBIE_BODYGROUP_HEADCRAB = 1;
}

ASourceNPCZombie::ASourceNPCZombie(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void ASourceNPCZombie::Spawn()
{
	// CNPC_BaseZombie::Spawn / CZombie::Spawn / SetZombieModel. The HL:A classic zombie converted by
	// Tools/ImportSource2Model.py is preferred when it has been generated; HL2's model is the fallback.
	if (!SetModel(TEXT("models/hla/zombie_classic.mdl")))
	{
		SetModel(TEXT("models/zombie/classic.mdl"));
	}
	if (Model && Model->HasModel())
	{
		const FSourceMDLFile* Mdl = Model->GetModel();
		HeadcrabBodyPart = Mdl->GetNumBodyParts() > ZOMBIE_BODYGROUP_HEADCRAB ? ZOMBIE_BODYGROUP_HEADCRAB : Mdl->FindBodyPart(TEXT("headcrab1"));
		if (HeadcrabBodyPart != INDEX_NONE)
		{
			Model->SetBodygroup(HeadcrabBodyPart, !bHeadless ? 1 : 0);	// SetBodygroup( ZOMBIE_BODYGROUP_HEADCRAB, !m_fIsHeadless )
		}
	}
	SetHull(13.0f, 72.0f);						// HULL_HUMAN
	ViewOffsetUnits = FVector3f(0, 0, 64);		// SetDefaultEyeOffset for a human hull
	FieldOfView = 0.2f;							// m_flFieldOfView
	BloodColor = ESourceBloodColor::Zombie;		// SetBloodColor( BLOOD_COLOR_ZOMBIE ) (HL2_EPISODIC)
	MaxHealth = Health = FSourceAmmoDef::Get().GetSkillValue(TEXT("sk_zombie_health"), 50.0f);
	MaxYawSpeedDeg = 25.0f;
	NPCState = ESourceNPCState::Idle;
	NextMoanTime = GetWorld() ? GetWorld()->GetTimeSeconds() + FMath::FRandRange(1.0f, 4.0f) : 0.0f;
	SetActivity(TEXT("ACT_IDLE"));
	UE_LOG(LogLambdaSource, Log, TEXT("npc_zombie: health %.0f, headcrab bodygroup %d, %d hitboxes"),
		Health, HeadcrabBodyPart, Model && Model->HasModel() ? Model->GetModel()->GetHitboxes().Num() : 0);
}

float ASourceNPCZombie::ZombieMaxYawSpeed() const
{
	// CNPC_BaseZombie::MaxYawSpeed
	const FString& Act = GetActivity();
	if (Act == TEXT("ACT_TURN_LEFT") || Act == TEXT("ACT_TURN_RIGHT")) { return 100.0f; }
	if (Act == TEXT("ACT_RUN")) { return 15.0f; }
	if (Act == TEXT("ACT_WALK") || Act == TEXT("ACT_IDLE")) { return 25.0f; }
	if (Act == TEXT("ACT_MELEE_ATTACK1") || Act == TEXT("ACT_MELEE_ATTACK2")) { return 120.0f; }
	return 90.0f;
}

void ASourceNPCZombie::NPCThink()
{
	UWorld* World = GetWorld();
	APawn* Player = GetPlayerPawn();
	const float Now = World ? World->GetTimeSeconds() : 0.0f;
	MaxYawSpeedDeg = ZombieMaxYawSpeed();

	// CZombie::PrescheduleThink: the classic zombie idles (moans) instead of a looping moan sound.
	if (Now > NextMoanTime && NPCState != ESourceNPCState::Idle)
	{
		IdleSound();
		NextMoanTime = Now + FMath::FRandRange(2.0f, 5.0f);
	}

	const bool bSeePlayer = Player && FInViewCone(Player->GetActorLocation()) && FVisible(Player);
	if (NPCState == ESourceNPCState::Idle)
	{
		if (bSeePlayer)
		{
			NPCState = ESourceNPCState::Combat;
			AlertSound();
		}
		else
		{
			if (GetActivity() != TEXT("ACT_IDLE")) { SetActivity(TEXT("ACT_IDLE")); }
			StopMoving();
			if (ShouldPlayIdleSound()) { IdleSound(); }
			return;
		}
	}
	if (!Player)
	{
		return;
	}

	// The attack animation owns the NPC until it is done; its events do the clawing.
	if (GetActivity() == TEXT("ACT_MELEE_ATTACK1") && !IsActivityFinished())
	{
		StopMoving();
		SetIdealYawToTarget(Player->GetActorLocation());
		return;
	}

	SetIdealYawToTarget(Player->GetActorLocation());
	const float Dist = DistanceUnits(Player);
	const FVector ToPlayer = (Player->GetActorLocation() - GetActorLocation()).GetSafeNormal2D();
	const float Dot = FVector::DotProduct(GetActorForwardVector(), ToPlayer);

	// CAI_BaseNPC::MeleeAttack1Conditions: within 64 units, facing (dot > 0.7), enemy on the ground.
	const ACharacter* PlayerChar = Cast<ACharacter>(Player);
	const bool bEnemyOnGround = !PlayerChar || !PlayerChar->GetCharacterMovement() || PlayerChar->GetCharacterMovement()->IsMovingOnGround();
	if (Dist <= 64.0f && Dot > 0.7f && bEnemyOnGround)
	{
		StopMoving();
		SetActivity(TEXT("ACT_MELEE_ATTACK1"));
		return;
	}

	// SCHED_CHASE_ENEMY: walk straight at the player - unless the hull is stuck against something, in which case
	// the pound animation owns the NPC until it finishes (and the block is re-tested after that).
	if (IsMovementBlocked())
	{
		const float NowTime = World ? World->GetTimeSeconds() : 0.0f;
		if (NowTime < NextPoundTime)
		{
			StopMoving();
			return;
		}
	}
	if (GetActivity() != TEXT("ACT_WALK"))
	{
		SetActivity(TEXT("ACT_WALK"));
	}
	SetMoveDirection(ToPlayer);
}

void ASourceNPCZombie::HandleAnimEvent(int32 EventId, const FString& EventName, const FString& Options)
{
	// CNPC_BaseZombie::HandleAnimEvent
	const FVector Forward = GetActorForwardVector();
	const FVector Right = GetActorRightVector();
	if (EventName == TEXT("AE_ZOMBIE_ATTACK_RIGHT"))
	{
		// right = right * 100; forward = forward * 200; QAngle qa( -15, -20, -10 ) — Source units, Source angles
		ClawAttack(ZOMBIE_MELEE_REACH, FSourceAmmoDef::Get().GetSkillValue(TEXT("sk_zombie_dmg_one_slash"), 10.0f),
			FRotator(-15.0f, -20.0f, -10.0f), FVector(200.0f, -100.0f, 0.0f));
		return;
	}
	if (EventName == TEXT("AE_ZOMBIE_ATTACK_LEFT"))
	{
		ClawAttack(ZOMBIE_MELEE_REACH, FSourceAmmoDef::Get().GetSkillValue(TEXT("sk_zombie_dmg_one_slash"), 10.0f),
			FRotator(-15.0f, 20.0f, -10.0f), FVector(200.0f, 100.0f, 0.0f));
		return;
	}
	if (EventName == TEXT("AE_ZOMBIE_ATTACK_BOTH"))
	{
		ClawAttack(ZOMBIE_MELEE_REACH, FSourceAmmoDef::Get().GetSkillValue(TEXT("sk_zombie_dmg_both_slash"), 25.0f),
			FRotator(45.0f, FMath::RandRange(-5, 5), FMath::RandRange(-5, 5)), FVector(200.0f, 0.0f, 0.0f));
		return;
	}
	if (EventName == TEXT("AE_ZOMBIE_ATTACK_SCREAM") || EventName == TEXT("AE_ZOMBIE_STARTSWAT"))
	{
		AttackSound();
		return;
	}
	if (EventName == TEXT("AE_ZOMBIE_STEP_LEFT") || EventName == TEXT("AE_NPC_LEFTFOOT")) { FootstepSound(false); return; }
	if (EventName == TEXT("AE_ZOMBIE_STEP_RIGHT") || EventName == TEXT("AE_NPC_RIGHTFOOT")) { FootstepSound(true); return; }
	if (EventName == TEXT("AE_ZOMBIE_SCUFF_LEFT")) { FootscuffSound(false); return; }
	if (EventName == TEXT("AE_ZOMBIE_SCUFF_RIGHT")) { FootscuffSound(true); return; }
	if (EventName == TEXT("AE_ZOMBIE_ALERTSOUND")) { AlertSound(); return; }
	if (EventName == TEXT("AE_ZOMBIE_POUND")) { EmitSound(TEXT("NPC_BaseZombie.PoundDoor")); return; }
	Super::HandleAnimEvent(EventId, EventName, Options);
}

void ASourceNPCZombie::ClawAttack(float DistUnits, float Damage, const FRotator& ViewPunch, const FVector& VelocityPunchUnits)
{
	// CNPC_BaseZombie::ClawAttack -> CheckTraceHullAttack( flDist, mins, maxs, iDamage, DMG_SLASH ): a hull the
	// width of ours (and as tall as it is wide) swept forward from our centre by the claw's reach.
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const float HalfWidth = GetCapsuleComponent()->GetScaledCapsuleRadius();
	const FVector Start = GetActorLocation();
	const FVector End = Start + GetActorForwardVector() * DistUnits * Scale;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ZombieClaw), false, this);
	FHitResult Hit;
	AActor* Hurt = nullptr;
	if (World->SweepSingleByChannel(Hit, Start, End, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeBox(FVector(HalfWidth)), Params))
	{
		Hurt = Hit.GetActor();
	}

	if (Hurt)
	{
		AttackHitSound();
		FHitResult DamageHit = Hit;
		if (DamageHit.ImpactPoint.IsZero()) { DamageHit.ImpactPoint = Hurt->GetActorLocation(); }
		FSourceDamageEvent Info(Damage, DamageHit, GetActorForwardVector(), UDamageType::StaticClass(), FVector::ZeroVector, DMG_SLASH);
		Hurt->TakeDamage(Damage, Info, GetController(), this);

		// pPlayer->ViewPunch( qaViewPunch ); pPlayer->VelocityPunch( vecVelocityPunch );
		if (ISourcePlayerPunch* Punch = Cast<ISourcePlayerPunch>(Hurt))
		{
			// QAngle(pitch, yaw, roll) in Source's sense -> UE rotator: pitch and yaw flip with the y mirror
			Punch->ViewPunch(FRotator(-ViewPunch.Pitch, -ViewPunch.Yaw, ViewPunch.Roll));
			const FVector Vel = GetActorForwardVector() * VelocityPunchUnits.X * Scale + GetActorRightVector() * VelocityPunchUnits.Y * Scale
				+ FVector::UpVector * VelocityPunchUnits.Z * Scale;
			Punch->VelocityPunch(Vel);
		}
	}
	else
	{
		AttackMissSound();
	}
}

void ASourceNPCZombie::OnMovementBlocked()
{
	// CZombie::OnObstructingDoor -> SelectDoorBash: a zombie that cannot get past something takes it out on the
	// obstruction rather than walking into it. Without the door-bashing schedule (and the interaction with
	// func_door) it just pounds; the important part is that it stops leaning into the geometry.
	UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.0f;
	if (Now < NextPoundTime)
	{
		return;
	}
	if (SetActivity(TEXT("ACT_ZOMBIE_WALLPOUND")) || SetActivity(TEXT("ACT_ZOMBIE_TANTRUM")))
	{
		NextPoundTime = Now + FMath::Max(0.5f, Model ? Model->GetSequenceDuration() : 1.0f);
	}
	else
	{
		SetActivity(TEXT("ACT_IDLE"));
		NextPoundTime = Now + 1.0f;
	}
}

void ASourceNPCZombie::IdleSound()
{
	// CZombie::IdleSound
	if (NPCState == ESourceNPCState::Idle && FMath::RandRange(0, 1) == 0)
	{
		return;
	}
	EmitSound(TEXT("Zombie.Idle"));
}

void ASourceNPCZombie::AlertSound()
{
	EmitSound(TEXT("Zombie.Alert"));
	NextMoanTime += FMath::FRandRange(2.0f, 4.0f);
}

float ASourceNPCZombie::GetHitgroupDamageMultiplier(int32 HitGroup, const FSourceDamageEvent& Info) const
{
	// CNPC_BaseZombie::GetHitgroupDamageMultiplier
	if (HitGroup == HITGROUP_HEAD)
	{
		if (Info.DamageType & DMG_BUCKSHOT)
		{
			float Dist = TNumericLimits<float>::Max();
			if (const APawn* Player = GetPlayerPawn())
			{
				Dist = DistanceUnits(Player);
			}
			if (Dist <= ZOMBIE_BUCKSHOT_TRIPLE_DAMAGE_DIST)
			{
				return 3.0f;
			}
		}
		else
		{
			return 2.0f;
		}
	}
	return Super::GetHitgroupDamageMultiplier(HitGroup, Info);
}

void ASourceNPCZombie::TraceAttack(const FSourceDamageEvent& Info)
{
	// CNPC_BaseZombie::TraceAttack
	if (Info.HitGroup == HITGROUP_HEAD)
	{
		bHeadShot = true;
	}
}

bool ASourceNPCZombie::IsHeavyDamage(float Damage, const FSourceDamageEvent& Info) const
{
	// CZombie::IsHeavyDamage (HL2_EPISODIC)
	if (Info.DamageType & DMG_BUCKSHOT)
	{
		if (Damage > MaxHealth / 3.0f)
		{
			return true;
		}
	}
	if (Info.DamageType & (DMG_BULLET | DMG_BUCKSHOT))
	{
		// !HasCondition(COND_CAN_MELEE_ATTACK1) && RandomFloat() > 0.5
		const APawn* Player = GetPlayerPawn();
		const bool bCanMelee = Player && DistanceUnits(Player) <= 64.0f;
		if (!bCanMelee && FMath::FRand() > 0.5f)
		{
			return true;
		}
	}
	return Super::IsHeavyDamage(Damage, Info);
}

void ASourceNPCZombie::OnTakeDamage_Alive(float Damage, AActor* Attacker, const FSourceDamageEvent& Info)
{
	// CNPC_BaseZombie::OnTakeDamage_Alive
	if (!bHeadShot && (Info.DamageType & DMG_BULLET) && !(Info.DamageType & (DMG_BUCKSHOT | DMG_SNIPER)))
	{
		Damage *= ZOMBIE_BULLET_DAMAGE_SCALE;
	}

	Super::OnTakeDamage_Alive(Damage, Attacker, Info);

	const float DamageThreshold = FMath::Min(1.0f, Damage / FMath::Max(MaxHealth, 1.0f));
	switch (ShouldReleaseHeadcrab(Info, DamageThreshold))
	{
	case ESourceHeadcrabRelease::Immediate:
		ReleaseHeadcrab(EyePosition(), FVector::ZeroVector, true, false, FVector::ZeroVector);
		break;
	case ESourceHeadcrabRelease::Ragdoll:
		ReleaseHeadcrab(EyePosition(), FVector::ZeroVector, true, true, Info.DamageForce * 0.25f);
		break;
	default:
		break;
	}
	bHeadShot = false;
}

ESourceHeadcrabRelease ASourceNPCZombie::ShouldReleaseHeadcrab(const FSourceDamageEvent& Info, float DamageThreshold) const
{
	// CNPC_BaseZombie::ShouldReleaseHeadcrab
	if (Health <= 0.0f && !bHeadless)
	{
		if (Info.DamageType & DMG_REMOVENORAGDOLL)
		{
			return ESourceHeadcrabRelease::No;
		}
		if (Info.DamageType & DMG_SNIPER)
		{
			return ESourceHeadcrabRelease::Ragdoll;
		}
		if (Info.DamageType & DMG_BULLET)
		{
			if (bHeadShot)
			{
				if (DamageThreshold > 0.25f)
				{
					return ESourceHeadcrabRelease::Ragdoll;
				}
			}
			else
			{
				return ESourceHeadcrabRelease::Immediate;
			}
		}
		if (Info.DamageType & DMG_BLAST)
		{
			return ESourceHeadcrabRelease::Ragdoll;
		}
	}
	return ESourceHeadcrabRelease::No;
}

void ASourceNPCZombie::ReleaseHeadcrab(const FVector& Origin, const FVector& VelocityCm, bool bRemoveHead, bool bRagdollCrab, const FVector& RagdollImpulse)
{
	// CNPC_BaseZombie::ReleaseHeadcrab
	ASourceBSPWorldActor* World = GetWorldActor();
	if (!World)
	{
		return;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	FVector Spot = Origin;
	Spot.Z -= 16.0f * Scale;	// if( !m_fIsTorso ) vecSpot.z -= 16;

	// SetHeadcrabSpawnLocation: the crab sits where the model's "headcrab" attachment is.
	FVector AttachLoc, AttachFwd;
	if (Model && Model->GetAttachmentWorld(TEXT("headcrab"), AttachLoc, AttachFwd))
	{
		Spot = AttachLoc;
	}

	ASourceNPCBase* Crab = Cast<ASourceNPCBase>(World->CreateNPC(TEXT("npc_headcrab"), Spot, GetActorRotation().Yaw));
	if (!Crab)
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("**npc_zombie: Can't make npc_headcrab!"));
		return;
	}

	if (bRagdollCrab)
	{
		// CreateRagGib( GetHeadcrabModel(), vecOrigin, angles, vecVelocity, 15 ): a dead headcrab, gone in 15 s.
		Crab->BecomeRagGib(RagdollImpulse, Crab->GetActorLocation(), 15.0f);
		// UTIL_BloodImpact( vecGibCenter, Vector(0,0,1), BLOOD_COLOR_YELLOW, 1 )
		SourceImpact::SpawnBlood(GetWorld(), MaterialLibrary, Crab->GetActorLocation(), FVector::UpVector, ESourceBloodColor::Yellow);
	}
	else
	{
		// pCrab->SetActivity( ACT_IDLE ); pCrab->SetAbsVelocity( vecVelocity ) - it wakes up on its own think.
		if (!VelocityCm.IsNearlyZero())
		{
			if (ACharacter* CrabChar = Cast<ACharacter>(Crab))
			{
				CrabChar->LaunchCharacter(VelocityCm, true, true);
			}
		}
	}

	if (bRemoveHead)
	{
		RemoveHead();
	}
}

void ASourceNPCZombie::RemoveHead()
{
	// CNPC_BaseZombie::RemoveHead -> m_fIsHeadless = true; SetZombieModel() -> SetBodygroup( ZOMBIE_BODYGROUP_HEADCRAB, 0 )
	bHeadless = true;
	if (Model && HeadcrabBodyPart != INDEX_NONE)
	{
		Model->SetBodygroup(HeadcrabBodyPart, 0);
	}
}
