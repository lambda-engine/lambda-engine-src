#include "Creatures/SourceNPCAntlion.h"

#include "Core/LambdaSourceModule.h"
#include "Core/LambdaSourceSettings.h"
#include "Core/SourceCoordinates.h"
#include "Formats/SourceMDLFile.h"
#include "Gameplay/SourceDamage.h"
#include "Gameplay/SourcePlayerPunch.h"
#include "Rendering/SourceStudioModelComponent.h"
#include "Weapons/SourceAmmoDef.h"

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"

using namespace SourceDamageType;

ASourceNPCAntlion::ASourceNPCAntlion(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void ASourceNPCAntlion::Spawn()
{
	// CNPC_Antlion::Spawn
	SetModel(TEXT("models/antlion.mdl"));
	SetHull(18.0f, 36.0f);						// HULL_MEDIUM: (-18,-18,0) to (18,18,36)
	ViewOffsetUnits = FVector3f(0, 0, 24);
	FieldOfView = 0.5f;							// m_flFieldOfView
	BloodColor = ESourceBloodColor::Yellow;		// SetBloodColor(BLOOD_COLOR_ANTLION)

	MaxHealth = Health = FSourceAmmoDef::Get().GetSkillValue(TEXT("sk_antlion_health"), 30.0f);
	SwipeDamage = FSourceAmmoDef::Get().GetSkillValue(TEXT("sk_antlion_swipe_damage"), 5.0f);
	JumpDamage = FSourceAmmoDef::Get().GetSkillValue(TEXT("sk_antlion_jump_damage"), 5.0f);

	// As with every Source NPC, how fast it runs is authored into the run animation's root motion.
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
	UE_LOG(LogLambdaSource, Log, TEXT("npc_antlion: health %.0f, swipe %.0f, pounce %.0f, run %.0f u/s, %d sequences"),
		Health, SwipeDamage, JumpDamage, RunSpeedUnits,
		Model && Model->HasModel() ? Model->GetModel()->GetSequences().Num() : 0);
}

void ASourceNPCAntlion::SetEnemy(AActor* NewEnemy)
{
	if (Enemy.Get() == NewEnemy)
	{
		return;
	}
	Enemy = NewEnemy;
	if (NewEnemy)
	{
		NPCState = ESourceNPCState::Combat;
		SetIdealYawToTarget(NewEnemy->GetActorLocation());
		AlertSound();
	}
	else
	{
		NPCState = ESourceNPCState::Alert;
	}
}

void ASourceNPCAntlion::UpdateEnemy()
{
	// CAI_Senses, cut to the one enemy this engine has: seen when close enough, inside the view cone and not
	// occluded, and remembered for the free-knowledge window after that.
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

bool ASourceNPCAntlion::CanPounce(float DistUnits) const
{
	// The band CNPC_Antlion::CheckRangeAttack1 will leap across: nearer than this it swipes, further it runs.
	if (DistUnits < ANTLION_JUMP_MIN || DistUnits > ANTLION_JUMP_MAX)
	{
		return false;
	}
	if (!Enemy.IsValid() || !FVisible(Enemy.Get()))
	{
		return false;
	}
	// Not up a cliff: the pounce is an arc, not a climb.
	const float Rise = (Enemy->GetActorLocation().Z - GetActorLocation().Z) / ULambdaSourceSettings::Get().UnitScale;
	return Rise < ANTLION_JUMP_MAX_RISE;
}

void ASourceNPCAntlion::NPCThink()
{
	UpdateEnemy();

	UCharacterMovementComponent* Move = GetCharacterMovement();
	AActor* Target = Enemy.Get();
	const float Now = GetWorld()->GetTimeSeconds();

	if (bMidJump)
	{
		// Airborne: nothing to decide until it lands, but it keeps facing where it is going.
		if (Move && !Move->Velocity.IsNearlyZero())
		{
			SetIdealYawToTarget(GetActorLocation() + Move->Velocity);
		}
		return;
	}

	if (bAttacking)
	{
		// The attack sequence is playing and its own events do the hitting; when it ends, face the enemy again.
		if (Target)
		{
			SetIdealYawToTarget(Target->GetActorLocation());
		}
		if (IsActivityFinished())
		{
			bAttacking = false;
			StopMoving();
			ClearPath();
			SetActivity(TEXT("ACT_IDLE"));
			WaitUntilTime = Now + FMath::FRandRange(0.0f, 0.3f);
		}
		return;
	}

	if (Now < WaitUntilTime)
	{
		return;
	}

	if (!Target)
	{
		StopMoving();
		SetActivity(TEXT("ACT_IDLE"));
		return;
	}

	const FVector EnemyPos = Target->GetActorLocation();
	SetIdealYawToTarget(EnemyPos);

	const FVector ToEnemy = (EnemyPos - GetActorLocation()).GetSafeNormal2D();
	const float FlDot = FVector::DotProduct(GetActorForwardVector().GetSafeNormal2D(), ToEnemy);
	const float FlDist = DistanceUnits(Target);

	if (Now - LastDebugLogTime > 1.0f)
	{
		LastDebugLogTime = Now;
		UE_LOG(LogLambdaSource, Verbose, TEXT("antlion think: dist=%.0fu dot=%.2f ground=%d act=%s"),
			FlDist, FlDot, IsOnGround() ? 1 : 0, *GetActivity());
	}

	if (Now < NextAttackTime || !IsOnGround())
	{
		ChaseEnemy();
		return;
	}

	// MeleeAttack1Conditions: in reach, and roughly facing.
	if (FlDist <= ANTLION_MELEE1_RANGE && FlDot > 0.7f)
	{
		StopMoving();
		ClearPath();
		bAttacking = true;
		SetActivity(TEXT("ACT_MELEE_ATTACK1"));
		return;
	}

	if (FlDot > 0.7f && CanPounce(FlDist))
	{
		StopMoving();
		ClearPath();
		bAttacking = true;
		StartJump();
		return;
	}

	ChaseEnemy();
}

void ASourceNPCAntlion::ChaseEnemy()
{
	AActor* Target = Enemy.Get();
	if (!Target)
	{
		return;
	}
	// Around what is in the way rather than into it - the same navmesh the headcrab chases on.
	const FVector Goal = FVisible(Target) ? Target->GetActorLocation() : LastKnownEnemyPos;
	SetActivity(TEXT("ACT_RUN"));
	NavigateTo(Goal);
}

void ASourceNPCAntlion::StartJump()
{
	// CNPC_Antlion::StartJump, solved the way the headcrab's leap is: the arc that lands on the enemy.
	AActor* Target = Enemy.Get();
	if (!Target)
	{
		return;
	}
	const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();
	const float Scale = Settings.UnitScale;
	const float Gravity = FMath::Max(1.0f, Settings.GravityUnits);

	const FVector3f Origin = FSourceCoords::ToSource(GetFeetLocation(), Scale);
	const FVector3f Goal = FSourceCoords::ToSource(Target->GetActorLocation(), Scale);

	// High enough to clear the ground between here and there, within what an antlion will attempt.
	const float Climb = Goal.Z - Origin.Z;
	const float Height = FMath::Clamp(Climb + 64.0f, 64.0f, ANTLION_JUMP_MAX_RISE);

	const float RiseTime = FMath::Sqrt(2.0f * Height / Gravity);
	const float FallHeight = FMath::Max(1.0f, Height - Climb);
	const float FallTime = FMath::Sqrt(2.0f * FallHeight / Gravity);
	const float Travel = FMath::Max(0.01f, RiseTime + FallTime);

	FVector3f Velocity;
	Velocity.X = (Goal.X - Origin.X) / Travel;
	Velocity.Y = (Goal.Y - Origin.Y) / Travel;
	Velocity.Z = Gravity * RiseTime;

	if (!SetActivity(TEXT("ACT_JUMP")))
	{
		SetActivity(TEXT("ACT_RANGE_ATTACK1"));
	}
	EmitSound(TEXT("NPC_Antlion.MeleeAttack"));

	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->SetMovementMode(MOVE_Falling);
	}
	// SetAbsVelocity: the launch takes it off the floor with that velocity.
	const FVector Launch(Velocity.X * Scale, -Velocity.Y * Scale, Velocity.Z * Scale);
	LaunchCharacter(Launch, true, true);
	bMidJump = true;
}

void ASourceNPCAntlion::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);
	if (!bMidJump)
	{
		return;
	}
	bMidJump = false;
	bAttacking = false;
	EmitSound(TEXT("NPC_Antlion.Land"));

	// A pounce that came down on the player hurts.
	if (Enemy.IsValid() && DistanceUnits(Enemy.Get()) <= ANTLION_MELEE1_RANGE)
	{
		MeleeAttack(ANTLION_MELEE1_RANGE, JumpDamage, FRotator(4.0f, 0.0f, 0.0f), FVector(-300.0f, 1.0f, 1.0f));
	}
	NextAttackTime = GetWorld()->GetTimeSeconds() + 0.5f;
}

void ASourceNPCAntlion::HandleAnimEvent(int32 EventId, const FString& EventName, const FString& Options)
{
	// CNPC_Antlion::HandleAnimEvent. The numbers are that file's: each swipe has its own view punch and shove.
	if (EventName.Contains(TEXT("AE_ANTLION_MELEE_HIT1")))
	{
		MeleeAttack(ANTLION_MELEE1_RANGE, SwipeDamage, FRotator(20.0f, 0.0f, -12.0f), FVector(-250.0f, 1.0f, 1.0f));
		return;
	}
	if (EventName.Contains(TEXT("AE_ANTLION_MELEE_HIT2")))
	{
		MeleeAttack(ANTLION_MELEE1_RANGE, SwipeDamage, FRotator(20.0f, 0.0f, 0.0f), FVector(-350.0f, 1.0f, 1.0f));
		return;
	}
	if (EventName.Contains(TEXT("AE_ANTLION_MELEE_POUNCE")))
	{
		MeleeAttack(ANTLION_MELEE1_RANGE, JumpDamage, FRotator(4.0f, 0.0f, 0.0f), FVector(-300.0f, 1.0f, 1.0f));
		return;
	}
	if (EventName.Contains(TEXT("AE_ANTLION_MELEE1_SOUND")))
	{
		EmitSound(TEXT("NPC_Antlion.MeleeAttackSingle"));
		return;
	}
	if (EventName.Contains(TEXT("AE_ANTLION_MELEE2_SOUND")))
	{
		EmitSound(TEXT("NPC_Antlion.MeleeAttackDouble"));
		return;
	}
	if (EventName.Contains(TEXT("AE_ANTLION_FOOTSTEP_SOFT")))
	{
		EmitSound(TEXT("NPC_Antlion.FootstepSoft"));
		return;
	}
	if (EventName.Contains(TEXT("AE_ANTLION_FOOTSTEP_HEAVY")))
	{
		EmitSound(TEXT("NPC_Antlion.FootstepHeavy"));
		return;
	}
	if (EventName.Contains(TEXT("AE_ANTLION_WALK_FOOTSTEP")))
	{
		EmitSound(TEXT("NPC_Antlion.Footstep"));
		return;
	}
	if (EventName.Contains(TEXT("AE_ANTLION_START_JUMP")))
	{
		return;		// the launch is StartJump's, not the animation's
	}

	Super::HandleAnimEvent(EventId, EventName, Options);
}

void ASourceNPCAntlion::MeleeAttack(float DistUnits, float Damage, const FRotator& ViewPunch, const FVector& VelocityPunchUnits)
{
	// CheckTraceHullAttack( distance, -Vector(16,16,32), Vector(16,16,32), damage, DMG_SLASH ): a hull swept
	// forward from the middle of the antlion by the reach of the swipe.
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const FVector Start = GetActorLocation();
	const FVector End = Start + GetActorForwardVector() * DistUnits * Scale;
	const FVector Extent(16.0f * Scale, 16.0f * Scale, 32.0f * Scale);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(AntlionMelee), false, this);
	FHitResult Hit;
	if (!World->SweepSingleByChannel(Hit, Start, End, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeBox(Extent), Params))
	{
		return;
	}
	AActor* Hurt = Hit.GetActor();
	if (!Hurt)
	{
		return;
	}

	if (Hit.ImpactPoint.IsZero())
	{
		Hit.ImpactPoint = Hurt->GetActorLocation();
	}
	FSourceDamageEvent Info(Damage, Hit, GetActorForwardVector(), UDamageType::StaticClass(), FVector::ZeroVector, DMG_SLASH);
	Hurt->TakeDamage(Damage, Info, GetController(), this);

	// pPlayer->ViewPunch( qa ); pPlayer->VelocityPunch( vec )
	if (ISourcePlayerPunch* Punch = Cast<ISourcePlayerPunch>(Hurt))
	{
		Punch->ViewPunch(FRotator(-ViewPunch.Pitch, -ViewPunch.Yaw, ViewPunch.Roll));
		const FVector Vel = GetActorForwardVector() * VelocityPunchUnits.X * Scale
			+ GetActorRightVector() * VelocityPunchUnits.Y * Scale
			+ FVector::UpVector * VelocityPunchUnits.Z * Scale;
		Punch->VelocityPunch(Vel);
	}
}
