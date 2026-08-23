#include "SourceNPCBase.h"
#include "LambdaMaterialLibrary.h"
#include "LambdaSoundLibrary.h"
#include "LambdaSourceModule.h"
#include "LambdaSourceSettings.h"
#include "SourceBSPWorldActor.h"
#include "SourceCoordinates.h"
#include "SourceStudioModelComponent.h"
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
	}
	// Only a player controller may possess this; there is no AIController behind it.
	AutoPossessAI = EAutoPossessAI::Disabled;

	Model = CreateDefaultSubobject<USourceStudioModelComponent>(TEXT("StudioModel"));
	Model->SetupAttachment(GetCapsuleComponent());
	Model->SetMobility(EComponentMobility::Movable);
	Model->SetCastShadow(true);

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
		Model->SetRelativeLocation(FVector(0, 0, -HullHalfHeightCm));
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
		UGameplayStatics::SpawnSoundAtLocation(this, Wave, GetActorLocation(), FRotator::ZeroRotator, Volume, Pitch);
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
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->StopMovementImmediately();
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

void ASourceNPCBase::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (NPCState == ESourceNPCState::None || NPCState == ESourceNPCState::Dead)
	{
		return;
	}

	UpdateYaw(DeltaSeconds);
	if (!MoveDirection.IsNearlyZero())
	{
		AddMovementInput(MoveDirection, 1.0f);
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
	Health -= DamageAmount;
	AActor* Attacker = EventInstigator ? EventInstigator->GetPawn() : DamageCauser;
	if (Health <= 0.0f)
	{
		Event_Killed(Attacker);
	}
	else
	{
		OnTakeDamage_Alive(DamageAmount, Attacker);
	}
	return DamageAmount;
}

void ASourceNPCBase::Event_Killed(AActor* Attacker)
{
	NPCState = ESourceNPCState::Dead;
	DeathSound();

	// Source turns the NPC into a ragdoll. Without physics to hand the body to, the corpse holds its death pose:
	// ACT_DIERAGDOLL where the model has one (a pose studiomdl writes for exactly this), else the last frame of
	// whatever was playing. It stops blocking the player but still takes hits, as a ragdoll would.
	if (!SetActivity(TEXT("ACT_DIERAGDOLL")))
	{
		SetActivity(TEXT("ACT_DIESIMPLE"));
	}
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->StopMovementImmediately();
		Move->DisableMovement();
	}
	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	SetLifeSpan(CorpseLifetime);
}
