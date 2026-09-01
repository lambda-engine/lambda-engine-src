#include "Creatures/SourceNPCBase.h"
#include "Core/LambdaStats.h"
#include "Core/LambdaStats.h"
#include "Materials/LambdaMaterialLibrary.h"
#include "Audio/LambdaSoundLibrary.h"
#include "Audio/SourceSoundScript.h"
#include "Sound/SoundAttenuation.h"
#include "Core/LambdaSourceModule.h"
#include "Core/LambdaSourceSettings.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "World/SourceBSPWorldActor.h"
#include "Weapons/SourceAmmoDef.h"
#include "Core/SourceCoordinates.h"
#include "Gameplay/SourceDamage.h"
#include "Formats/SourcePHYFile.h"
#include "Entities/SourcePropPhysics.h"
#include "Rendering/SourceRagdoll.h"
#include "Rendering/SourceStudioModelComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "Kismet/GameplayStatics.h"

ASourceNPCBase::ASourceNPCBase(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = true;

	// Yaw is ours to drive (CAI_Motor), not the controller's or the movement direction's.
	bUseControllerRotationYaw = false;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->bOrientRotationToMovement = false;
		Move->bUseControllerDesiredRotation = false;
		// No controller drives an NPC (Source's AI is not a controller); the movement component must still run.
		Move->bRunPhysicsWithNoController = true;
		Move->MaxAcceleration = 4096.0f;	// NPCs reach their ground speed at once, as Source's motor does
		Move->BrakingDecelerationWalking = 4096.0f;
		Move->GroundFriction = 8.0f;
		Move->bCanWalkOffLedges = true;
		Move->SetWalkableFloorAngle(45.0f);
		// Unreal's own physics interaction shoves objects with a force of its own choosing; an NPC's shadow pushes
		// what it walks into at its own pace and no harder, through ShadowPush - same as the player.
		Move->bEnablePhysicsInteraction = false;
	}
	// Only a player controller may possess this; there is no AIController behind it.
	AutoPossessAI = EAutoPossessAI::Disabled;

	// UE's Pawn profile ignores the Visibility channel (so camera traces pass through pawns); our bullets trace that
	// channel, and Source's MASK_SHOT most certainly hits NPCs. Block it.
	GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);

	Model = CreateDefaultSubobject<USourceStudioModelComponent>(TEXT("StudioModel"));
	Model->SetupAttachment(GetCapsuleComponent());
	Model->SetMobility(EComponentMobility::Movable);
	Model->SetCastShadow(true);
	// World decals are projected boxes in UE and would paint the NPC standing in its own blood pool; Source's
	// world decals live on the brush surface only (model decals are a separate system, not ported).
	Model->SetReceivesDecals(false);

	// Hide the placeholder skeletal mesh a character comes with; the studio model is the body.
	if (USkeletalMeshComponent* Skel = GetMesh())
	{
		Skel->SetVisibility(false);
		Skel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

void ASourceNPCBase::InitializeFromEntity(const FSourceEntity& InEntity, ASourceBSPWorldActor* InWorldActor, ULambdaMaterialLibrary* Materials)
{
	Entity = InEntity;
	WorldActor = InWorldActor;
	MaterialLibrary = Materials;

	Spawn();

	// A Source entity's origin is its feet; the capsule's location is its centre.
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	FVector3f Origin = FVector3f::ZeroVector;
	Entity.GetVector(TEXT("origin"), Origin);
	FVector3f Angles = FVector3f::ZeroVector;
	Entity.GetVector(TEXT("angles"), Angles);

	const FVector Feet = FSourceCoords::ToUE(Origin, Scale);
	SetActorLocation(Feet + FVector(0, 0, HullHalfHeightCm), false, nullptr, ETeleportType::TeleportPhysics);
	const FRotator Rot = FSourceCoords::AnglesToUE(Angles);
	SetActorRotation(FRotator(0.0f, Rot.Yaw, 0.0f));
	IdealYaw = Rot.Yaw;

	// MOVETYPE_STEP: on the ground under gravity. Without a controller the component never leaves MOVE_None on its own.
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->SetMovementMode(MOVE_Walking);
	}

	NPCState = ESourceNPCState::Idle;
}

bool ASourceNPCBase::SetModel(const FString& ModelPath)
{
	RegisterAsNavInvoker();
	if (!Model)
	{
		return false;
	}
	if (!MaterialLibrary)
	{
		MaterialLibrary = NewObject<ULambdaMaterialLibrary>(this);
		MaterialLibrary->Initialize();
	}
	if (!Model->SetModel(ModelPath, MaterialLibrary))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("%s: could not load model '%s'"), *Entity.ClassName, *ModelPath);
		return false;
	}
	Model->OnAnimationEvent.AddUObject(this, &ASourceNPCBase::OnModelAnimationEvent);
	return true;
}

void ASourceNPCBase::SetHull(float HalfWidthUnits, float HeightUnits)
{

	// A Source hull is a box from (-w,-w,0) to (w,w,h) around the feet. The capsule stands in for it: same
	// height, radius from the half width. The model hangs from the capsule centre down to the feet.
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const float Radius = HalfWidthUnits * Scale;
	HullHalfHeightCm = FMath::Max(HeightUnits * Scale * 0.5f, Radius);
	GetCapsuleComponent()->SetCapsuleSize(Radius, HullHalfHeightCm);
	if (Model)
	{
		// Source authors NPC models to fit their hull; an imported one need not. Half-Life: Alyx's zombie leans a
		// long way forward of its origin, so with the hull kept at Source's size (a fatter hull would not fit
		// through corridors the player does) the model is drawn back by however far it overhangs the front. Only
		// the facing side moves: the hull is unchanged, and what the NPC walks into stops at its chest, not at
		// its waist with its head through the wall.
		float ModelForwardOffsetCm = 0.0f;
		if (Model->HasModel())
		{
			const float Overhang = Model->GetModel()->GetHullMax().X - HalfWidthUnits;
			if (Overhang > 0.5f)
			{
				// A hull's worth of shift is as far as this goes; beyond that the model would visibly stand
				// away from where it is.
				ModelForwardOffsetCm = FMath::Min(Overhang, HalfWidthUnits) * Scale;
				UE_LOG(LogLambdaSource, Log, TEXT("%s: '%s' overhangs its %.0f unit hull by %.0f units; drawing it that far back"),
					*Entity.ClassName, *Model->GetModelPath(), HalfWidthUnits, Overhang);
			}
		}
		Model->SetRelativeLocation(FVector(-ModelForwardOffsetCm, 0, -HullHalfHeightCm));
	}
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		// Step size follows the hull like Source's sv_stepsize does for the player.
		Move->MaxStepHeight = FMath::Min(18.0f * Scale, HullHalfHeightCm);
	}
}

FVector ASourceNPCBase::GetFeetLocation() const
{
	return GetActorLocation() - FVector(0, 0, HullHalfHeightCm);
}

FVector ASourceNPCBase::EyePosition() const
{
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	// The view offset is in the NPC's local frame (forward, left, up), so rotate it with the yaw.
	const FVector Local = FSourceCoords::ToUE(ViewOffsetUnits, Scale);
	return GetFeetLocation() + GetActorRotation().RotateVector(Local);
}

bool ASourceNPCBase::FVisible(const AActor* Target) const
{
	UWorld* World = GetWorld();
	if (!World || !Target)
	{
		return false;
	}
	FVector TargetEyes;
	FRotator Unused;
	Target->GetActorEyesViewPoint(TargetEyes, Unused);

	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(NPCVisible), /*bTraceComplex=*/ true, this);
	Params.AddIgnoredActor(Target);
	// Anything solid between the eyes blocks sight.
	return !World->LineTraceSingleByChannel(Hit, EyePosition(), TargetEyes, ECC_Visibility, Params);
}

bool ASourceNPCBase::FInViewCone(const FVector& WorldPos) const
{
	FVector To = WorldPos - GetActorLocation();
	To.Z = 0.0f;
	if (!To.Normalize())
	{
		return true;
	}
	FVector Forward = GetActorForwardVector();
	Forward.Z = 0.0f;
	Forward.Normalize();
	return FVector::DotProduct(Forward, To) > FieldOfView;
}

float ASourceNPCBase::DistanceUnits(const AActor* Target) const
{
	return Target ? (float)(FVector::Dist(GetActorLocation(), Target->GetActorLocation()) / ULambdaSourceSettings::Get().UnitScale) : BIG_NUMBER;
}

void ASourceNPCBase::EmitSound(const FString& SoundScript)
{
	float Volume = 1.0f, Pitch = 1.0f;
	if (ULambdaSoundWave* Wave = FLambdaSoundCache::Get().CreateWaveResolved(this, SoundScript, false, Volume, Pitch))
	{
		// The soundscript's soundlevel decides how far the sound carries (CBaseEntity::EmitSound passes it to the
		// engine as the channel's attenuation).
		const FSourceSoundScriptEntry* Entry = FSourceSoundScripts::Get().Find(SoundScript);
		USoundAttenuation* Attenuation = FLambdaSoundCache::Get().GetAttenuationForSoundLevel(Entry ? Entry->SoundLevel : 75.0f);
		UGameplayStatics::SpawnSoundAtLocation(this, Wave, GetActorLocation(), FRotator::ZeroRotator, Volume, Pitch,
			0.0f, Attenuation);
	}
}

bool ASourceNPCBase::HaveSequenceForActivity(const FString& ActivityName) const
{
	return Model && Model->HasModel() && Model->GetModel()->SelectWeightedSequence(ActivityName) != INDEX_NONE;
}

bool ASourceNPCBase::SetActivity(const FString& ActivityName)
{
	if (!Model || !Model->HasModel())
	{
		return false;
	}
	// CAI_BaseNPC::SetActivity: re-requesting the activity that is already playing does not restart it unless it
	// has run out (a looping idle keeps looping; a finished one-shot plays again).
	if (CurrentActivity.Equals(ActivityName, ESearchCase::IgnoreCase) && !IsActivityFinished())
	{
		return true;
	}
	if (!Model->PlayActivity(ActivityName))
	{
		return false;
	}
	CurrentActivity = ActivityName;

	// CAI_Motor moves at the sequence's ground speed (GetSequenceGroundSpeed -> GetIdealSpeed): a zombie's walk
	// animation carries 45 units/s of root motion, so that is how fast it walks. Sequences without motion (idle,
	// attacks) leave the last speed alone; movement is stopped separately.
	const float GroundSpeedUnits = Model->GetSequenceGroundSpeed();
	if (GroundSpeedUnits > 0.0f)
	{
		if (UCharacterMovementComponent* Move = GetCharacterMovement())
		{
			Move->MaxWalkSpeed = GroundSpeedUnits * ULambdaSourceSettings::Get().UnitScale;
		}
	}
	return true;
}

bool ASourceNPCBase::IsActivityFinished() const
{
	return !Model || !Model->HasModel() || Model->GetSequence() == INDEX_NONE || Model->IsSequenceFinished();
}

void ASourceNPCBase::SetIdealYawToTarget(const FVector& WorldPos)
{
	FVector To = WorldPos - GetActorLocation();
	To.Z = 0.0f;
	if (!To.IsNearlyZero())
	{
		IdealYaw = To.Rotation().Yaw;
	}
}

float ASourceNPCBase::DeltaIdealYaw() const
{
	return FMath::FindDeltaAngleDegrees(GetActorRotation().Yaw, IdealYaw);
}

void ASourceNPCBase::UpdateYaw(float DeltaSeconds)
{
	// CAI_Motor::UpdateYaw turns at yawSpeed * 10 degrees per second - MaxYawSpeed is in the AI's own units.
	const float Step = MaxYawSpeedDeg * 10.0f * DeltaSeconds;
	const float Delta = DeltaIdealYaw();
	if (FMath::Abs(Delta) <= Step)
	{
		SetActorRotation(FRotator(0.0f, IdealYaw, 0.0f));
	}
	else
	{
		SetActorRotation(FRotator(0.0f, GetActorRotation().Yaw + FMath::Sign(Delta) * Step, 0.0f));
	}
}

void ASourceNPCBase::StopMoving()
{
	MoveDirection = FVector::ZeroVector;
	// CAI_Motor stops a ground move; a hull in the air stays ballistic. StopMovementImmediately zeroes the whole
	// velocity, and called every think it would reset a falling NPC's descent ten times a second - which is how a
	// headcrab nudged off the player's head used to float down.
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		if (Move->IsMovingOnGround())
		{
			Move->StopMovementImmediately();
		}
	}
}

bool ASourceNPCBase::IsOnGround() const
{
	const UCharacterMovementComponent* Move = GetCharacterMovement();
	return Move && Move->IsMovingOnGround();
}

APawn* ASourceNPCBase::GetPlayerPawn() const
{
	return UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
}

const FString& ASourceNPCBase::GetSurfaceProp() const
{
	static const FString Empty;
	return (Model && Model->HasModel()) ? Model->GetModel()->GetSurfaceProp() : Empty;
}

bool ASourceNPCBase::ShouldPlayIdleSound() const
{
	return (NPCState == ESourceNPCState::Idle || NPCState == ESourceNPCState::Alert) && FMath::RandRange(0, 99) == 0;
}

void ASourceNPCBase::NotifyHit(UPrimitiveComponent* MyComp, AActor* Other, UPrimitiveComponent* OtherComp, bool bSelfMoved,
	FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit)
{
	Super::NotifyHit(MyComp, Other, OtherComp, bSelfMoved, HitLocation, HitNormal, NormalImpulse, Hit);
	ASourcePropPhysics::ShadowPush(Hit.GetComponent(), Hit, GetVelocity(), *GetClassName());
}

void ASourceNPCBase::Tick(float DeltaSeconds)
{
	SCOPE_CYCLE_COUNTER(STAT_LambdaNPCThink);
	Super::Tick(DeltaSeconds);
	if (bDyingWithAnim && NPCState == ESourceNPCState::Dead)
	{
		// the death animation has finished; now the physics gets the body (no fresh kick - it already fell)
		if (IsActivityFinished())
		{
			bDyingWithAnim = false;
			if (BecomeRagdoll(FVector::ZeroVector, GetActorLocation()))
			{
				GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			}
		}
		return;
	}
	if (NPCState == ESourceNPCState::None || NPCState == ESourceNPCState::Dead)
	{
		return;
	}

	UpdateYaw(DeltaSeconds);
	if (!MoveDirection.IsNearlyZero())
	{
		AddMovementInput(MoveDirection, 1.0f);

		// Pushed but not moving: something is in the way. A quarter of a second of it is a block, not a stumble.
		const UCharacterMovementComponent* Move = GetCharacterMovement();
		const float Speed = Move ? Move->Velocity.Size2D() : 0.0f;
		const float Wanted = Move ? Move->MaxWalkSpeed : 0.0f;
		if (Wanted > 0.0f && Speed < Wanted * 0.25f)
		{
			BlockedTime += DeltaSeconds;
			if (BlockedTime > 0.25f && !bMovementBlocked)
			{
				bMovementBlocked = true;
				StopMoving();
				OnMovementBlocked();
			}
		}
		else
		{
			BlockedTime = 0.0f;
			bMovementBlocked = false;
		}
	}
	else
	{
		BlockedTime = 0.0f;
	}
	// If it's been a while since we did a full flinch, forget that we flinched so we'll flinch fully again
	if (bFlinchedMemory && GetWorld()->GetTimeSeconds() > NextFlinchTime)
	{
		bFlinchedMemory = false;
	}

	ThinkAccumulator += DeltaSeconds;
	while (ThinkAccumulator >= ThinkInterval)
	{
		ThinkAccumulator -= ThinkInterval;
		if (ShouldPlayIdleSound())
		{
			IdleSound();
		}
		NPCThink();
		if (NPCState == ESourceNPCState::Dead)
		{
			break;
		}
	}
}

void ASourceNPCBase::OnModelAnimationEvent(int32 EventId, const FString& EventName, const FString& Options)
{
	HandleAnimEvent(EventId, EventName, Options);
}

void ASourceNPCBase::HandleAnimEvent(int32 EventId, const FString& EventName, const FString& Options)
{
	// AE_CL_PLAYSOUND: play the soundscript named in the options (footsteps and the like).
	if (EventName.Equals(TEXT("AE_CL_PLAYSOUND"), ESearchCase::IgnoreCase) && !Options.IsEmpty())
	{
		EmitSound(Options);
	}
}

float ASourceNPCBase::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	if (NPCState == ESourceNPCState::Dead || DamageAmount <= 0.0f)
	{
		return 0.0f;
	}
	AActor* Attacker = EventInstigator ? EventInstigator->GetPawn() : DamageCauser;

	// The CTakeDamageInfo: ours carries force, position, type and hit group; a plain point event just the point.
	FSourceDamageEvent Info;
	Info.Damage = DamageAmount;
	Info.DamagePosition = GetActorLocation();
	if (DamageEvent.IsOfType(FSourceDamageEvent::ClassID))
	{
		Info = static_cast<const FSourceDamageEvent&>(DamageEvent);
		Info.Damage = DamageAmount;
	}
	else if (DamageEvent.IsOfType(FPointDamageEvent::ClassID))
	{
		const FPointDamageEvent& Point = static_cast<const FPointDamageEvent&>(DamageEvent);
		Info.HitInfo = Point.HitInfo;
		Info.ShotDirection = Point.ShotDirection;
		Info.DamagePosition = Point.HitInfo.ImpactPoint;
	}

	// CAI_BaseNPC::TraceAttack: the hit group scales the damage (head shots hurt), then the NPC's own hook.
	LastHitGroup = Info.HitGroup;
	float Damage = DamageAmount * GetHitgroupDamageMultiplier(Info.HitGroup, Info);
	TraceAttack(Info);

	LastDamageForce = CalcDamageForceVector(Damage, Info.DamageForce, Attacker);
	LastDamagePosition = Info.DamagePosition;

	// CBaseCombatCharacter::OnTakeDamage: OnTakeDamage_Alive applies it (and derived classes scale it first), then
	// Event_Killed if that was the end - in that order, so a zombie releases its headcrab before it ragdolls.
	const float HealthBefore = Health;
	OnTakeDamage_Alive(Damage, Attacker, Info);
	UE_LOG(LogLambdaSource, Verbose, TEXT("%s took %.1f (x%.1f for hitgroup %d, type %d): health %.1f -> %.1f"),
		*Entity.ClassName, DamageAmount, GetHitgroupDamageMultiplier(Info.HitGroup, Info), Info.HitGroup, Info.DamageType, HealthBefore, Health);
	if (Health <= 0.0f && NPCState != ESourceNPCState::Dead)
	{
		Event_Killed(Attacker);
	}
	return Damage;
}

void ASourceNPCBase::OnTakeDamage_Alive(float Damage, AActor* Attacker, const FSourceDamageEvent& Info)
{
	Health -= Damage;
	if (Health > 0.0f)
	{
		// CAI_BaseNPC::CheckFlinches: heavy damage takes a full-body flinch when the model has one, else a gesture.
		if (IsHeavyDamage(Damage, Info) && HaveSequenceForActivity(GetFlinchActivity(true, false)) && CanFlinch())
		{
			SetActivity(GetFlinchActivity(true, false));
			NextFlinchTime = GetWorld()->GetTimeSeconds() + FMath::FRandRange(0.5f, 1.0f);
		}
		else
		{
			PlayFlinchGesture();
		}
	}
}

float ASourceNPCBase::GetHitgroupDamageMultiplier(int32 HitGroup, const FSourceDamageEvent& Info) const
{
	FSourceAmmoDef& Skill = FSourceAmmoDef::Get();
	switch (HitGroup)
	{
	case SourceHitGroup::HITGROUP_GENERIC: return 1.0f;
	case SourceHitGroup::HITGROUP_HEAD: return Skill.GetSkillValue(TEXT("sk_npc_head"), 3.0f);
	case SourceHitGroup::HITGROUP_CHEST: return Skill.GetSkillValue(TEXT("sk_npc_chest"), 1.0f);
	case SourceHitGroup::HITGROUP_STOMACH: return Skill.GetSkillValue(TEXT("sk_npc_stomach"), 1.0f);
	case SourceHitGroup::HITGROUP_LEFTARM:
	case SourceHitGroup::HITGROUP_RIGHTARM: return Skill.GetSkillValue(TEXT("sk_npc_arm"), 1.0f);
	case SourceHitGroup::HITGROUP_LEFTLEG:
	case SourceHitGroup::HITGROUP_RIGHTLEG: return Skill.GetSkillValue(TEXT("sk_npc_leg"), 1.0f);
	default: return 1.0f;
	}
}

FString ASourceNPCBase::GetFlinchActivity(bool bHeavyDamage, bool bGesture) const
{
	// CAI_BaseNPC::GetFlinchActivity
	FString Flinch;
	switch (LastHitGroup)
	{
	case SourceHitGroup::HITGROUP_HEAD: Flinch = bGesture ? TEXT("ACT_GESTURE_FLINCH_HEAD") : TEXT("ACT_FLINCH_HEAD"); break;
	case SourceHitGroup::HITGROUP_STOMACH: Flinch = bGesture ? TEXT("ACT_GESTURE_FLINCH_STOMACH") : TEXT("ACT_FLINCH_STOMACH"); break;
	case SourceHitGroup::HITGROUP_LEFTARM: Flinch = bGesture ? TEXT("ACT_GESTURE_FLINCH_LEFTARM") : TEXT("ACT_FLINCH_LEFTARM"); break;
	case SourceHitGroup::HITGROUP_RIGHTARM: Flinch = bGesture ? TEXT("ACT_GESTURE_FLINCH_RIGHTARM") : TEXT("ACT_FLINCH_RIGHTARM"); break;
	case SourceHitGroup::HITGROUP_LEFTLEG: Flinch = bGesture ? TEXT("ACT_GESTURE_FLINCH_LEFTLEG") : TEXT("ACT_FLINCH_LEFTLEG"); break;
	case SourceHitGroup::HITGROUP_RIGHTLEG: Flinch = bGesture ? TEXT("ACT_GESTURE_FLINCH_RIGHTLEG") : TEXT("ACT_FLINCH_RIGHTLEG"); break;
	case SourceHitGroup::HITGROUP_CHEST: Flinch = bGesture ? TEXT("ACT_GESTURE_FLINCH_CHEST") : TEXT("ACT_FLINCH_CHEST"); break;
	default:
		Flinch = bHeavyDamage ? (bGesture ? TEXT("ACT_GESTURE_BIG_FLINCH") : TEXT("ACT_BIG_FLINCH"))
			: (bGesture ? TEXT("ACT_GESTURE_SMALL_FLINCH") : TEXT("ACT_SMALL_FLINCH"));
		break;
	}
	if (!HaveSequenceForActivity(Flinch))
	{
		if (bHeavyDamage)
		{
			Flinch = bGesture ? TEXT("ACT_GESTURE_BIG_FLINCH") : TEXT("ACT_BIG_FLINCH");
			if (!HaveSequenceForActivity(Flinch))
			{
				Flinch = bGesture ? TEXT("ACT_GESTURE_SMALL_FLINCH") : TEXT("ACT_SMALL_FLINCH");
			}
		}
		else
		{
			Flinch = bGesture ? TEXT("ACT_GESTURE_SMALL_FLINCH") : TEXT("ACT_SMALL_FLINCH");
		}
	}
	return Flinch;
}

bool ASourceNPCBase::CanFlinch() const
{
	return GetWorld() && NextFlinchTime < GetWorld()->GetTimeSeconds();
}

void ASourceNPCBase::PlayFlinchGesture()
{
	// CAI_BaseNPC::PlayFlinchGesture
	if (!CanFlinch() || !Model)
	{
		return;
	}
	float FlNextFlinch = FMath::FRandRange(0.5f, 1.0f);
	FString Flinch;
	// If I haven't flinched for a while, play the big flinch gesture
	if (!bFlinchedMemory)
	{
		Flinch = GetFlinchActivity(true, true);
		if (HaveSequenceForActivity(Flinch))
		{
			Model->PlayGesture(Flinch);
		}
		else
		{
			Flinch.Reset();
		}
		bFlinchedMemory = true;
	}
	else
	{
		Flinch = GetFlinchActivity(false, true);
		if (HaveSequenceForActivity(Flinch))
		{
			Model->PlayGesture(Flinch);
		}
		else
		{
			Flinch.Reset();
		}
	}
	if (!Flinch.IsEmpty())
	{
		// Get the duration of the flinch and delay the next one by that (plus a bit more)
		FlNextFlinch += Model->GetGestureDuration(Flinch);
		NextFlinchTime = GetWorld()->GetTimeSeconds() + FlNextFlinch;
	}
}

FString ASourceNPCBase::GetDeathActivity() const
{
	// CAI_BaseNPC::GetDeathActivity picks by wound; the arm/leg ones are private activities of the HL:A imports.
	switch (LastHitGroup)
	{
	case SourceHitGroup::HITGROUP_HEAD: return TEXT("ACT_DIE_HEADSHOT");
	case SourceHitGroup::HITGROUP_CHEST: return TEXT("ACT_DIE_CHESTSHOT");
	case SourceHitGroup::HITGROUP_STOMACH: return TEXT("ACT_DIE_GUTSHOT");
	case SourceHitGroup::HITGROUP_LEFTARM: return TEXT("ACT_DIE_LEFTARM");
	case SourceHitGroup::HITGROUP_RIGHTARM: return TEXT("ACT_DIE_RIGHTARM");
	case SourceHitGroup::HITGROUP_LEFTLEG: return TEXT("ACT_DIE_LEFTLEG");
	case SourceHitGroup::HITGROUP_RIGHTLEG: return TEXT("ACT_DIE_RIGHTLEG");
	default: return TEXT("ACT_DIESIMPLE");
	}
}

bool ASourceNPCBase::HasHitboxes() const
{
	return Model && Model->HasHitboxes();
}

bool ASourceNPCBase::TraceHitboxes(const FVector& Start, const FVector& End, FSourceHitboxHit& OutHit) const
{
	return Model && Model->TraceHitboxes(Start, End, OutHit);
}

void ASourceNPCBase::BecomeRagGib(const FVector& ForceImpulse, const FVector& ForcePosition, float Lifetime)
{
	NPCState = ESourceNPCState::Dead;
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->StopMovementImmediately();
		Move->DisableMovement();
	}
	CorpseLifetime = Lifetime;
	SetLifeSpan(Lifetime);
	if (BecomeRagdoll(ForceImpulse, ForcePosition))
	{
		GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	else
	{
		GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	}
}

void ASourceNPCBase::Event_Killed(AActor* Attacker)
{
	NPCState = ESourceNPCState::Dead;
	DeathSound();

	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->StopMovementImmediately();
		Move->DisableMovement();
	}
	SetLifeSpan(CorpseLifetime);

	// Models with per-wound death animations (the HL:A imports) play the one for the hit group and hand the body
	// to physics when it ends; HL2's NPCs ragdoll on the spot.
	const FString DeathActivity = GetDeathActivity();
	if (!DeathActivity.IsEmpty() && HaveSequenceForActivity(DeathActivity))
	{
		SetActivity(DeathActivity);
		bDyingWithAnim = true;
		GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		return;
	}

	// CBaseCombatCharacter::Event_Killed: ragdoll unless gibbed. The body is handed to physics in the pose it died
	// in, kicked by the killing blow (CalcDamageForceVector), and the hull stops being solid.
	if (BecomeRagdoll(LastDamageForce, LastDamagePosition))
	{
		GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return;
	}

	// No collision model: the corpse holds its death pose - ACT_DIERAGDOLL where the model has one (a pose
	// studiomdl writes for exactly this), else the last frame playing. It stops blocking the player but still
	// takes hits, as a ragdoll would.
	if (!SetActivity(TEXT("ACT_DIERAGDOLL")))
	{
		SetActivity(TEXT("ACT_DIESIMPLE"));
	}
	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
}

ASourceRagdoll* ASourceNPCBase::BecomeRagdollSilent()
{
	if (NPCState == ESourceNPCState::Dead)
	{
		return Ragdoll.Get();
	}
	NPCState = ESourceNPCState::Dead;
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->StopMovementImmediately();
		Move->DisableMovement();
	}
	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// No force: the tongue holds it up, it does not get thrown anywhere.
	BecomeRagdoll(FVector::ZeroVector, GetActorLocation());
	return Ragdoll.Get();
}

bool ASourceNPCBase::BecomeRagdoll(const FVector& ForceImpulse, const FVector& ForcePosition)
{
	if (!Model || !Model->HasModel() || Ragdoll.IsValid())
	{
		return false;
	}
	FSourcePHYFile Phy;
	FString Error;
	if (!Phy.Load(Model->GetModelPath(), ULambdaSourceSettings::Get().UnitScale, &Error))
	{
		UE_LOG(LogLambdaSource, Log, TEXT("%s: no ragdoll (%s)"), *Entity.ClassName, *Error);
		return false;
	}
	ASourceRagdoll* NewRagdoll = ASourceRagdoll::Create(GetWorld(), Model, Phy, ForceImpulse, ForcePosition,
		GetVelocity(), BloodColor, CorpseLifetime);
	if (!NewRagdoll)
	{
		return false;
	}
	Ragdoll = NewRagdoll;
	return true;
}

FVector ASourceNPCBase::CalcDamageForceVector(float Damage, const FVector& GivenForce, AActor* Attacker) const
{
	// Already have a damage force in the data, use that.
	if (!GivenForce.IsNearlyZero())
	{
		return GivenForce;
	}
	if (!Attacker)
	{
		return FVector::ZeroVector;
	}
	// Calculate an impulse large enough to push a 75kg man 4 in/sec per point of damage
	const float ForceScale = Damage * 75.0f * 4.0f;
	FVector Force = GetActorLocation() - Attacker->GetActorLocation();
	Force.Normalize();
	return Force * ForceScale * 2.54f;	// kg*cm/s
}

void ASourceNPCBase::RegisterAsNavInvoker()
{
	// So that a route exists under an NPC that is far from the player - navigation is generated around whoever
	// asks for it, and an NPC pathing home is asking.
	if (UWorld* World = GetWorld())
	{
		if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
		{
			NavSys->RegisterNavigationInvoker(this, 2000.0f, 3500.0f);
		}
	}
}

void ASourceNPCBase::ClearPath()
{
	PathPoints.Reset();
	PathCorner = 0;
	NextRepathTime = 0.0f;
}

bool ASourceNPCBase::NavigateTo(const FVector& Goal)
{
	UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.0f;
	const float Scale = ULambdaSourceSettings::Get().UnitScale;

	// How near a corner counts as reached, and how far the goal may drift before the route is stale. Both in
	// Hammer units so they read the way the rest of the AI does.
	const float CornerReached = 24.0f * Scale;
	const float GoalMoved = 48.0f * Scale;

	const bool bStale = PathPoints.Num() == 0
		|| Now >= NextRepathTime
		|| FVector::DistSquared2D(Goal, PathGoal) > FMath::Square(GoalMoved);

	if (bStale)
	{
		PathPoints.Reset();
		PathCorner = 0;
		PathGoal = Goal;

		// Put the destination on the navmesh before asking for a route to it. A designer-placed node sits
		// where it looks right, not where the mesh happens to be, and a goal an inch off the mesh has no
		// route to it at all - which is how a guard post nobody could reach silently became a soldier
		// standing still. The extent is the one the cover search uses; taller and it finds the floor above.
		if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
		{
			FNavLocation Projected;
			if (NavSys->ProjectPointToNavigation(PathGoal, Projected, FVector(150.0f, 150.0f, 180.0f)))
			{
				PathGoal = Projected.Location;
			}
		}
		// Half a second is enough that a walking enemy does not outrun the route, and rare enough that a room
		// full of NPCs is not re-planning every frame.
		NextRepathTime = Now + 0.5f;

		if (UNavigationSystemV1* NavSys = World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr)
		{
			if (UNavigationPath* Path = NavSys->FindPathToLocationSynchronously(World, GetActorLocation(), PathGoal, this))
			{
				if (Path->IsValid() && !Path->IsPartial() && Path->PathPoints.Num() > 1)
				{
					PathPoints = Path->PathPoints;
					// The first point is where we already are.
					PathCorner = 1;
				}
				UE_LOG(LogLambdaSource, Verbose, TEXT("%s: route to %s - %d corners%s"), *GetClassName(),
					*Goal.ToCompactString(), Path->PathPoints.Num(),
					Path->IsPartial() ? TEXT(" (partial, ignored)") : TEXT(""));
			}
		}
	}

	// Walk the corners we have already reached.
	while (PathPoints.IsValidIndex(PathCorner)
		&& FVector::DistSquared2D(GetActorLocation(), PathPoints[PathCorner]) < FMath::Square(CornerReached))
	{
		++PathCorner;
	}

	if (PathPoints.IsValidIndex(PathCorner))
	{
		FVector Dir = PathPoints[PathCorner] - GetActorLocation();
		Dir.Z = 0.0f;
		if (Dir.Normalize())
		{
			SetMoveDirection(Dir);
			// Face the way we are walking, as CAI_Motor's move execute sets ideal yaw from the motion. Without
			// this the yaw stays wherever the last FaceToward left it - at the enemy - and a soldier running to
			// a flank walks backwards the whole way. An action that wants eyes on something while standing
			// still calls FaceToward after this, in the same think, and wins.
			SetIdealYawToTarget(GetActorLocation() + Dir);
			return true;
		}
	}

	// No navmesh, no route, or the route has run out: head straight at it, which is what this did before there
	// was any navigation at all. That counts as a move - it used to return false, so MindMoveTo cancelled the
	// very direction this had just set and the fallback could never once carry anybody. If the straight line
	// runs into a wall the blocked detection ends it in a quarter of a second, which is the honest way for an
	// unreachable destination to fail: visibly, and quickly.
	FVector Straight = Goal - GetActorLocation();
	Straight.Z = 0.0f;
	if (Straight.Normalize())
	{
		SetMoveDirection(Straight);
		SetIdealYawToTarget(GetActorLocation() + Straight);
		return true;
	}
	return false;
}
