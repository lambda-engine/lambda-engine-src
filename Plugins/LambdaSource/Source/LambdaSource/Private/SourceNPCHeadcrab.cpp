#include "SourceNPCHeadcrab.h"
#include "LambdaSourceModule.h"
#include "LambdaSourceSettings.h"
#include "SourceAmmoDef.h"
#include "SourceCoordinates.h"
#include "SourceStudioModelComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/DamageEvents.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"

ASourceNPCHeadcrab::ASourceNPCHeadcrab(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	GetCapsuleComponent()->OnComponentHit.AddDynamic(this, &ASourceNPCHeadcrab::OnCapsuleHit);
}

void ASourceNPCHeadcrab::Spawn()
{
	// CHeadcrab::Spawn + CBaseHeadcrab::Spawn
	SetModel(TEXT("models/headcrabclassic.mdl"));
	SetHull(12.0f, 24.0f);						// HULL_TINY: (-12,-12,0) to (12,12,24)
	ViewOffsetUnits = FVector3f(6, 0, 11);		// SetViewOffset: position of the eyes relative to the origin
	FieldOfView = 0.5f;							// m_flFieldOfView
	BloodColor = ESourceBloodColor::Green;		// SetBloodColor(BLOOD_COLOR_GREEN)
	MaxHealth = Health = FSourceAmmoDef::Get().GetSkillValue(TEXT("sk_headcrab_health"), 10.0f);
	MeleeDamage = FSourceAmmoDef::Get().GetSkillValue(TEXT("sk_headcrab_melee_dmg"), 5.0f);

	// The run speed is authored into the ACT_RUN animation's root motion, as it is for every Source NPC.
	if (Model && Model->HasModel())
	{
		const int32 RunSeq = Model->GetModel()->SelectWeightedSequence(TEXT("ACT_RUN"));
		RunSpeedUnits = RunSeq != INDEX_NONE ? Model->GetModel()->GetSequenceGroundSpeed(RunSeq) : 0.0f;
	}
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->MaxWalkSpeed = RunSpeedUnits * ULambdaSourceSettings::Get().UnitScale;
	}

	SetActivity(TEXT("ACT_IDLE"));
	UE_LOG(LogLambdaSource, Log, TEXT("npc_headcrab: health %.0f, bite %.0f, run %.0f u/s, %d sequences"),
		Health, MeleeDamage, RunSpeedUnits, Model && Model->HasModel() ? Model->GetModel()->GetSequences().Num() : 0);
}

void ASourceNPCHeadcrab::SetEnemy(AActor* NewEnemy)
{
	if (Enemy.Get() == NewEnemy)
	{
		return;
	}
	Enemy = NewEnemy;
	if (NewEnemy)
	{
		// SCHED_HEADCRAB_WAKE_ANGRY(_NO_DISPLAY): stop, face, TASK_SOUND_WAKE, and the threat display if the
		// model has one (the classic headcrab does not; headcrab.mdl's "rearup" is ACT_HEADCRAB_THREAT_DISPLAY).
		NPCState = ESourceNPCState::Combat;
		AlertSound();
		SetIdealYawToTarget(NewEnemy->GetActorLocation());
		if (HaveSequenceForActivity(TEXT("ACT_HEADCRAB_THREAT_DISPLAY")))
		{
			SetActivity(TEXT("ACT_HEADCRAB_THREAT_DISPLAY"));
		}
		LastKnownEnemyPos = NewEnemy->GetActorLocation();
		LastSeenEnemyTime = GetWorld()->GetTimeSeconds();
	}
	else
	{
		NPCState = ESourceNPCState::Alert;
	}
}

void ASourceNPCHeadcrab::UpdateEnemy()
{
	// CAI_Senses: the player is the only enemy we can have. Seen when within look distance, inside the view cone
	// and not occluded; remembered for the free-knowledge window after that.
	APawn* Player = GetPlayerPawn();
	const float Now = GetWorld()->GetTimeSeconds();
	if (Player && DistanceUnits(Player) <= LookDistUnits && FInViewCone(Player->GetActorLocation()) && FVisible(Player))
	{
		LastKnownEnemyPos = Player->GetActorLocation();
		LastSeenEnemyTime = Now;
		if (!Enemy.IsValid())
		{
			SetEnemy(Player);
		}
	}
	else if (Enemy.IsValid() && Now - LastSeenEnemyTime > ENEMY_FREE_KNOWLEDGE)
	{
		SetEnemy(nullptr);
	}
}

ASourceNPCHeadcrab::EAttackCondition ASourceNPCHeadcrab::RangeAttack1Conditions(float FlDot, float FlDistUnits) const
{
	// CBaseHeadcrab::RangeAttack1Conditions
	if (GetWorld()->GetTimeSeconds() < NextAttackTime)
	{
		return EAttackCondition::NotReady;
	}
	if (!IsOnGround())
	{
		return EAttackCondition::NotReady;
	}
	if (FlDot < 0.65f)
	{
		return EAttackCondition::NotFacing;
	}
	// This code stops lots of headcrabs swarming you and blocking you whilst jumping up and down in your face
	// over and over. It forces them to back up a bit.
	if (FlDistUnits < HEADCRAB_MIN_JUMP_DIST)
	{
		return EAttackCondition::TooClose;
	}
	if (FlDistUnits > HEADCRAB_MAX_JUMP_DIST)
	{
		return EAttackCondition::TooFar;
	}
	// Make sure the way is clear!
	if (Enemy.IsValid() && !FVisible(Enemy.Get()))
	{
		return EAttackCondition::Blocked;
	}
	return EAttackCondition::CanAttack;
}

void ASourceNPCHeadcrab::NPCThink()
{
	UpdateEnemy();

	// Make the crab coo a little bit in combat state (CBaseHeadcrab::PrescheduleThink).
	if (NPCState == ESourceNPCState::Combat && FMath::FRandRange(0.0f, 5.0f) < 0.1f)
	{
		IdleSound();
	}

	UCharacterMovementComponent* Move = GetCharacterMovement();
	AActor* Target = Enemy.Get();

	if (bMidJump)
	{
		// ThrowThink: airborne, nothing to decide until we land. Face the way we are flying.
		if (Move && !Move->Velocity.IsNearlyZero())
		{
			SetIdealYawToTarget(GetActorLocation() + Move->Velocity);
		}
		return;
	}

	if (bAttacking)
	{
		// TASK_RANGE_ATTACK1: the jumpattack sequence is playing; its events do the work. When it ends:
		// TASK_SET_ACTIVITY ACT_IDLE, TASK_FACE_IDEAL, TASK_WAIT_RANDOM 0.5.
		if (Target)
		{
			SetIdealYawToTarget(Target->GetActorLocation());
		}
		if (IsActivityFinished())
		{
			bAttacking = false;
			StopMoving();
			SetActivity(TEXT("ACT_IDLE"));
			if (bAttackFailed)
			{
				// our attack failed because we just ran into something solid. delay attacking for a while so
				// we don't just repeatedly leap at the enemy from a bad location.
				bAttackFailed = false;
				NextAttackTime = GetWorld()->GetTimeSeconds() + 1.2f;
			}
			WaitUntilTime = GetWorld()->GetTimeSeconds() + FMath::FRandRange(0.0f, 0.5f);
		}
		return;
	}

	if (GetWorld()->GetTimeSeconds() < WaitUntilTime)
	{
		return;
	}

	if (!Target)
	{
		// NPC_STATE_IDLE / ALERT: stand. Without the node graph there is no SCHED_PATROL_WALK to run.
		StopMoving();
		SetActivity(TEXT("ACT_IDLE"));
		return;
	}

	// NPC_STATE_COMBAT
	const FVector EnemyPos = Target->GetActorLocation();
	SetIdealYawToTarget(EnemyPos);

	FVector ToEnemy = EnemyPos - GetActorLocation();
	ToEnemy.Z = 0.0f;
	ToEnemy.Normalize();
	FVector Forward = GetActorForwardVector();
	Forward.Z = 0.0f;
	Forward.Normalize();
	const float FlDot = FVector::DotProduct(Forward, ToEnemy);
	const float FlDist = DistanceUnits(Target);
	const EAttackCondition Cond = RangeAttack1Conditions(FlDot, FlDist);

	// npc_headcrab debug trace, once a second: what the attack decision saw.
	if (GetWorld()->GetTimeSeconds() - LastDebugLogTime > 1.0f)
	{
		LastDebugLogTime = GetWorld()->GetTimeSeconds();
		UE_LOG(LogLambdaSource, Verbose, TEXT("headcrab think: dist=%.0fu dot=%.2f ground=%d cond=%d act=%s mode=%d vel=%s"),
			FlDist, FlDot, IsOnGround() ? 1 : 0, (int32)Cond, *GetActivity(),
			Move ? (int32)Move->MovementMode.GetValue() : -1, Move ? *Move->Velocity.ToString() : TEXT("-"));
	}

	switch (Cond)
	{
	case EAttackCondition::CanAttack:
		// SCHED_HEADCRAB_RANGE_ATTACK1: TASK_STOP_MOVING, TASK_FACE_ENEMY, TASK_RANGE_ATTACK1
		StopMoving();
		bAttacking = true;
		bCommittedToJump = false;
		SetActivity(TEXT("ACT_RANGE_ATTACK1"));
		break;

	case EAttackCondition::TooClose:
		// SCHED_BACK_AWAY_FROM_ENEMY
		BackAwayFromEnemy();
		break;

	case EAttackCondition::NotFacing:
		// SCHED_COMBAT_FACE: turn in place (ACT_TURN_LEFT/RIGHT if authored, else idle) until facing.
		StopMoving();
		SetActivity(DeltaIdealYaw() > 0.0f ? TEXT("ACT_TURN_LEFT") : TEXT("ACT_TURN_RIGHT")) || SetActivity(TEXT("ACT_IDLE"));
		break;

	case EAttackCondition::TooFar:
	case EAttackCondition::Blocked:
		// SCHED_CHASE_ENEMY - in a straight line; pathfinding is not ported.
		ChaseEnemy();
		break;

	case EAttackCondition::NotReady:
	default:
		StopMoving();
		SetActivity(TEXT("ACT_IDLE"));
		break;
	}
}

void ASourceNPCHeadcrab::ChaseEnemy()
{
	AActor* Target = Enemy.Get();
	if (!Target)
	{
		return;
	}
	const FVector Goal = FVisible(Target) ? Target->GetActorLocation() : LastKnownEnemyPos;
	FVector Dir = Goal - GetActorLocation();
	Dir.Z = 0.0f;
	if (Dir.Normalize())
	{
		SetActivity(TEXT("ACT_RUN"));
		SetMoveDirection(Dir);
	}
}

void ASourceNPCHeadcrab::BackAwayFromEnemy()
{
	AActor* Target = Enemy.Get();
	if (!Target)
	{
		return;
	}
	FVector Away = GetActorLocation() - Target->GetActorLocation();
	Away.Z = 0.0f;
	if (Away.Normalize())
	{
		SetActivity(TEXT("ACT_RUN"));
		SetMoveDirection(Away);
	}
}

void ASourceNPCHeadcrab::HandleAnimEvent(int32 EventId, const FString& EventName, const FString& Options)
{
	// CBaseHeadcrab::HandleAnimEvent
	if (EventName.Equals(TEXT("AE_HEADCRAB_JUMPATTACK"), ESearchCase::IgnoreCase))
	{
		// Ignore if we're in mid air
		if (bMidJump)
		{
			return;
		}
		AActor* Target = Enemy.Get();
		if (Target)
		{
			if (bCommittedToJump)
			{
				JumpAttack(false, CommittedJumpPos);
			}
			else
			{
				// Jump at my enemy's eyes.
				FVector Eyes;
				FRotator Unused;
				Target->GetActorEyesViewPoint(Eyes, Unused);
				JumpAttack(false, Eyes);
			}
			bCommittedToJump = false;
		}
		else
		{
			// Jump hop, don't care where.
			JumpAttack(true, FVector::ZeroVector);
		}
		return;
	}
	if (EventName.Equals(TEXT("AE_HEADCRAB_JUMP_TELEGRAPH"), ESearchCase::IgnoreCase))
	{
		TelegraphSound();
		if (AActor* Target = Enemy.Get())
		{
			// Once we telegraph, we MUST jump. This is also when we commit to what point we jump at.
			FRotator Unused;
			Target->GetActorEyesViewPoint(CommittedJumpPos, Unused);
			bCommittedToJump = true;
		}
		return;
	}
	Super::HandleAnimEvent(EventId, EventName, Options);
}

void ASourceNPCHeadcrab::JumpAttack(bool bRandomJump, const FVector& WorldPos)
{
	// CBaseHeadcrab::JumpAttack, in Source units: the ballistic solve for a leap that lands on vecPos.
	const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();
	const float Scale = Settings.UnitScale;
	FVector3f VecJumpVel;

	if (!bRandomJump)
	{
		float Gravity = Settings.GravityUnits;
		if (Gravity <= 1.0f)
		{
			Gravity = 1.0f;
		}

		const FVector3f Origin = FSourceCoords::ToSource(GetFeetLocation(), Scale);
		const FVector3f VecPos = FSourceCoords::ToSource(WorldPos, Scale);

		// How fast does the headcrab need to travel to reach the position given gravity?
		const float FlActualHeight = VecPos.Z - Origin.Z;
		float Height = FlActualHeight;
		if (Height < 16.0f)
		{
			Height = 16.0f;
		}
		else
		{
			const float FlMaxHeight = 120.0f;	// 400 when thrown
			if (Height > FlMaxHeight)
			{
				Height = FlMaxHeight;
			}
		}

		// overshoot the jump by an additional 8 inches
		float AdditionalHeight = 0.0f;
		if (Height < 32.0f)
		{
			AdditionalHeight = 8.0f;
		}
		Height += AdditionalHeight;

		// NOTE: This equation here is from vf^2 = vi^2 + 2*a*d
		const float Speed = FMath::Sqrt(2.0f * Gravity * Height);
		float Time = Speed / Gravity;

		// add in the time it takes to fall the additional height
		Time += FMath::Sqrt((2.0f * AdditionalHeight) / Gravity);

		// Scale the sideways velocity to get there at the right time
		VecJumpVel = (VecPos - Origin) / Time;

		// Speed to offset gravity at the desired height.
		VecJumpVel.Z = Speed;

		// Don't jump too far/fast.
		const float FlJumpSpeed = VecJumpVel.Size();
		const float FlMaxSpeed = 650.0f;	// 1000 when thrown
		if (FlJumpSpeed > FlMaxSpeed)
		{
			VecJumpVel *= FlMaxSpeed / FlJumpSpeed;
		}
	}
	else
	{
		// Jump hop, don't care where.
		const FVector Fwd = GetActorForwardVector();
		VecJumpVel = FVector3f((float)Fwd.X, (float)-Fwd.Y, 1.0f) * 350.0f;
	}

	AttackSound();
	Leap(VecJumpVel);
}

void ASourceNPCHeadcrab::Leap(const FVector3f& VelocityUnits)
{
	// CBaseHeadcrab::Leap
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	IgnoreWorldCollisionTime = GetWorld()->GetTimeSeconds() + HEADCRAB_IGNORE_WORLD_COLLISION_TIME;
	bMidJump = true;

	// SetGroundEntity(NULL) + SetAbsVelocity: a launch takes the character off the floor with that velocity.
	const FVector Vel(VelocityUnits.X * Scale, -VelocityUnits.Y * Scale, VelocityUnits.Z * Scale);
	LaunchCharacter(Vel, true, true);
}

void ASourceNPCHeadcrab::OnCapsuleHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit)
{
	// SetTouch(&CBaseHeadcrab::LeapTouch) is armed by Leap; anything we bump into while airborne goes through it.
	if (bMidJump && OtherActor && OtherActor != this)
	{
		LeapTouch(OtherActor);
	}
}

void ASourceNPCHeadcrab::LeapTouch(AActor* Other)
{
	// CBaseHeadcrab::LeapTouch
	const bool bHatesOther = Other == GetPlayerPawn();	// IRelationType(pOther) == D_HT
	if (bHatesOther)
	{
		bMidJump = false;
		// Don't hit if back on ground
		if (!IsOnGround())
		{
			BiteSound();
			TouchDamage(Other);
			// attack succeeded, so don't delay our next attack if we previously thought we failed
			bAttackFailed = false;
		}
		return;
	}

	if (!IsOnGround())
	{
		// Still in the air... just ran into something solid, so the attack probably failed. Headcrabs try to
		// ignore the world for a fraction of a second after they jump, because they often brush doorframes or
		// props as they leap.
		if (GetWorld()->GetTimeSeconds() < IgnoreWorldCollisionTime)
		{
			return;
		}
		bAttackFailed = true;
	}
	bMidJump = false;
}

void ASourceNPCHeadcrab::TouchDamage(AActor* Other)
{
	// CalcDamageInfo: sk_headcrab_melee_dmg, DMG_SLASH
	FPointDamageEvent DamageEvent;
	DamageEvent.Damage = MeleeDamage;
	DamageEvent.ShotDirection = GetActorForwardVector();
	Other->TakeDamage(MeleeDamage, DamageEvent, GetController(), this);
}

void ASourceNPCHeadcrab::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);
	// Back on the ground: the leap is over whether or not it connected (RunTask TASK_RANGE_ATTACK1 on finish).
	bMidJump = false;
}

void ASourceNPCHeadcrab::OnTakeDamage_Alive(float Damage, AActor* Attacker, const FSourceDamageEvent& Info)
{
	Super::OnTakeDamage_Alive(Damage, Attacker, Info);
	if (Health <= 0.0f)
	{
		return;
	}
	PainSound();
	// Being hurt by someone we had not noticed is how a headcrab usually wakes up (COND_LIGHT_DAMAGE -> combat).
	if (!Enemy.IsValid() && Attacker && Attacker == GetPlayerPawn())
	{
		SetEnemy(Attacker);
	}
	// SCHED_SMALL_FLINCH in the alert state, if the model has a flinch (headcrab.mdl does, the classic does not).
	if (NPCState == ESourceNPCState::Alert && HaveSequenceForActivity(TEXT("ACT_FLINCH")))
	{
		SetActivity(TEXT("ACT_FLINCH"));
	}
}
