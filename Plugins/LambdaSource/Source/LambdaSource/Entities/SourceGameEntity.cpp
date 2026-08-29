#include "Entities/SourceGameEntity.h"

#include "Core/LambdaSourceModule.h"
#include "Core/SourceCoordinates.h"
#include "Audio/LambdaSoundLibrary.h"
#include "Game/LambdaGameDll.h"
#include "Kismet/GameplayStatics.h"

ASourceGameEntity::ASourceGameEntity()
{
	PrimaryActorTick.bCanEverTick = true;
}

void ASourceGameEntity::InitializeFromEntity(const FSourceBSPFile& Map, int32 ModelIndex, const FSourceEntity& InEntity,
	ULambdaMaterialLibrary* MaterialLibrary, ASourceBSPWorldActor* InWorldActor)
{
	Super::InitializeFromEntity(Map, ModelIndex, InEntity, MaterialLibrary, InWorldActor);

	// The behaviour is made after the geometry, because the first thing most entities do on Spawn is measure
	// themselves - a button works out how far it can travel from the size of its own brush.
	Behaviour = FLambdaGameDll::Get().CreateEntity(InEntity.ClassName, this, GameId);
	if (!Behaviour)
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("%s: the game module claimed this class but made no entity for it"),
			*InEntity.ClassName);
		return;
	}
	Behaviour->Spawn();
}

void ASourceGameEntity::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Behaviour)
	{
		Behaviour->Destroy();
		FLambdaGameDll::Get().DestroyEntity(Behaviour, GameId);
		Behaviour = nullptr;
		GameId = lambda::InvalidEntity;
	}
	Super::EndPlay(Reason);
}

void ASourceGameEntity::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bMoving && bMovingAxis)
	{
		if (StepAxisMove(DeltaSeconds))
		{
			bMoving = false;
			bMovingAxis = false;
			StopLoopingSoundNow();
			if (Behaviour)
			{
				Behaviour->OnMoveDone(lambda::MoveResult::Arrived);
			}
		}
		else
		{
			CheckBlocked();
		}
	}
	else if (bMoving)
	{
		// LinearMove / AngularMove: constant speed towards the destination, and the last step lands exactly on
		// it rather than overshooting - a door that stops a hair past its frame never quite closes.
		const FVector3f Current = bMovingAngular ? GetSourceAngles() : GetSourceOrigin();
		const FVector3f ToTarget = MoveTarget - Current;
		const float Remaining = ToTarget.Size();
		const float Step = MoveSpeed * DeltaSeconds;
		if (Remaining <= Step || Remaining < KINDA_SMALL_NUMBER)
		{
			bMovingAngular ? SetSourceAngles(MoveTarget) : SetSourceOrigin(MoveTarget);
			bMoving = false;
			StopLoopingSoundNow();
			if (Behaviour)
			{
				Behaviour->OnMoveDone(lambda::MoveResult::Arrived);
			}
		}
		else
		{
			const FVector3f Next = Current + ToTarget / Remaining * Step;
			bMovingAngular ? SetSourceAngles(Next) : SetSourceOrigin(Next);
			CheckBlocked();
		}
	}

	if (Behaviour)
	{
		Behaviour->Think(DeltaSeconds);
	}
}

void ASourceGameEntity::BeginLinearMove(const FVector3f& Destination, float Speed)
{
	MoveTarget = Destination;
	MoveSpeed = FMath::Max(1.0f, Speed);
	bMoving = true;
	bMovingAngular = false;		// cleared, or a slide after a turn would keep turning
	bMovingAxis = false;
}

void ASourceGameEntity::BeginAngularMove(const FVector3f& DestinationAngles, float Speed)
{
	MoveTarget = DestinationAngles;
	MoveSpeed = FMath::Max(1.0f, Speed);
	bMoving = true;
	bMovingAngular = true;
	bMovingAxis = false;
}

void ASourceGameEntity::BeginAxisMove(const FVector3f& AxisPoint, const FVector3f& AxisDir,
	const FVector3f& DestinationOrigin, const FVector3f& DestinationAngles, float Speed)
{
	const FVector Unit = FSourceCoords::ToUEDirection(AxisDir);

	// No axis is no hinge. Rather than turn about an arbitrary one, go nowhere and report arrival: a door
	// with a broken hinge stays shut instead of tearing itself out of its frame.
	if (Unit.IsNearlyZero())
	{
		bMoving = false;
		bMovingAxis = false;
		if (Behaviour)
		{
			Behaviour->OnMoveDone(lambda::MoveResult::Arrived);
		}
		return;
	}

	AxisUnitUE = Unit;
	AxisPivotUE = FSourceCoords::ToUE(AxisPoint, FSourceCoords::GetUnitScale());
	AxisTargetRotation = FSourceCoords::AnglesToUE(DestinationAngles).Quaternion();
	AxisTargetOrigin = DestinationOrigin;

	MoveSpeed = FMath::Max(1.0f, Speed);
	bMoving = true;
	bMovingAngular = false;
	bMovingAxis = true;
}

bool ASourceGameEntity::StepAxisMove(float DeltaSeconds)
{
	const FQuat Current = FSourceCoords::AnglesToUE(GetSourceAngles()).Quaternion();

	// How far round the axis the target still is. Taking it from where the entity is now, every tick, is what
	// makes a swing interrupted and sent back turn through the part it travelled rather than the whole of it.
	const FQuat Delta = AxisTargetRotation * Current.Inverse();
	const float Remaining = FMath::RadiansToDegrees(
		2.0f * FMath::Atan2(FVector::DotProduct(FVector(Delta.X, Delta.Y, Delta.Z), AxisUnitUE), Delta.W));

	const float Step = MoveSpeed * DeltaSeconds;

	// The last step lands exactly on the target rather than overshooting - a door that stops a hair past its
	// frame never quite closes.
	if (FMath::Abs(Remaining) <= Step || FMath::Abs(Remaining) < KINDA_SMALL_NUMBER)
	{
		SetSourceAngles(FSourceCoords::AnglesFromUE(AxisTargetRotation.Rotator()));
		SetSourceOrigin(AxisTargetOrigin);
		return true;
	}

	const FQuat Turn(AxisUnitUE, FMath::DegreesToRadians(Remaining < 0.0f ? -Step : Step));

	// Back through Source angles rather than straight onto the actor: SourceAngles is what GetAngles answers
	// and what the next move starts from, so it has to stay the truth about where the entity is pointing.
	SetSourceAngles(FSourceCoords::AnglesFromUE((Turn * Current).Rotator()));

	// A hinge that misses the entity's own origin carries it around the line, so the same turn is applied to
	// where it is as to which way it faces. One that runs through the origin leaves this a no-op.
	const float Scale = FSourceCoords::GetUnitScale();
	const FVector Position = FSourceCoords::ToUE(GetSourceOrigin(), Scale);
	SetSourceOrigin(FSourceCoords::ToSource(AxisPivotUE + Turn.RotateVector(Position - AxisPivotUE), Scale));

	return false;
}

void ASourceGameEntity::SetSolidity(bool bSolid)
{
	if (BrushMesh)
	{
		BrushMesh->SetCollisionEnabled(bSolid ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
	}
}

void ASourceGameEntity::SetTriggerVolume(bool bTrigger)
{
	if (!BrushMesh)
	{
		return;
	}
	bIsTriggerVolume = bTrigger;
	if (!bTrigger)
	{
		BrushMesh->ClearCollisionConvexMeshes();
		BrushMesh->bUseComplexAsSimpleCollision = true;
		BrushMesh->OnComponentBeginOverlap.RemoveDynamic(this, &ASourceGameEntity::HandleBeginOverlap);
		BrushMesh->OnComponentEndOverlap.RemoveDynamic(this, &ASourceGameEntity::HandleEndOverlap);
		BrushMesh->SetGenerateOverlapEvents(false);
		BrushMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		BrushMesh->SetCollisionResponseToAllChannels(ECR_Block);
		return;
	}

	// Tested against the brush volumes, not the triangles. The triangles are a hollow shell: somebody standing
	// in the middle of a trigger touches none of them, so the trigger sees him arrive as he crosses the near
	// wall and leave again a moment later, and never knows he is inside. This is the shape Source collides
	// against - one convex element per brush, so an L-shaped trigger is still L-shaped.
	BrushMesh->bUseComplexAsSimpleCollision = false;
	BrushMesh->SetCollisionConvexMeshes(BrushHulls);
	if (BrushHulls.Num() == 0)
	{
		// No brushes to work from (a model built entirely of displacements, or a BSP missing its brush lumps).
		// The bounds of what was drawn are a poor trigger, but a great deal better than none.
		UE_LOG(LogLambdaSource, Warning, TEXT("%s: no brushes to make a volume from, falling back to its bounds"),
			*Entity.ClassName);
		TArray<FVector> Corners;
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			Corners.Add(FVector(
				(Corner & 1) ? LocalBounds.Max.X : LocalBounds.Min.X,
				(Corner & 2) ? LocalBounds.Max.Y : LocalBounds.Min.Y,
				(Corner & 4) ? LocalBounds.Max.Z : LocalBounds.Min.Z));
		}
		BrushMesh->AddCollisionConvexMesh(MoveTemp(Corners));
	}

	// Queries only, overlapping everything: a trigger is passed through, never stood on. The brush is still
	// drawn or not on its own merits - a tools/toolstrigger texture is nodraw and disappears the usual way,
	// without this having to know about it.
	BrushMesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	BrushMesh->SetCollisionResponseToAllChannels(ECR_Overlap);
	BrushMesh->SetGenerateOverlapEvents(true);
	BrushMesh->OnComponentBeginOverlap.AddDynamic(this, &ASourceGameEntity::HandleBeginOverlap);
	BrushMesh->OnComponentEndOverlap.AddDynamic(this, &ASourceGameEntity::HandleEndOverlap);
}

void ASourceGameEntity::HandleBeginOverlap(UPrimitiveComponent* /*OverlappedComp*/, AActor* Other,
	UPrimitiveComponent* /*OtherComp*/, int32 /*OtherBodyIndex*/, bool /*bFromSweep*/, const FHitResult& /*Sweep*/)
{
	if (Behaviour && Other && Other != this)
	{
		Behaviour->OnStartTouch(FLambdaGameDll::Get().IdForEntity(Other));
	}
}

void ASourceGameEntity::HandleEndOverlap(UPrimitiveComponent* /*OverlappedComp*/, AActor* Other,
	UPrimitiveComponent* /*OtherComp*/, int32 /*OtherBodyIndex*/)
{
	if (Behaviour && Other && Other != this)
	{
		Behaviour->OnEndTouch(FLambdaGameDll::Get().IdForEntity(Other));
	}
}

void ASourceGameEntity::StartLoopingSound(const FString& SoundName)
{
	StopLoopingSoundNow();
	if (SoundName.IsEmpty())
	{
		return;
	}
	float Volume = 1.0f, Pitch = 1.0f;
	// Looping, because the caller is asking for a sound that lasts as long as the move does.
	if (ULambdaSoundWave* Wave = FLambdaSoundCache::Get().CreateWaveResolved(this, SoundName, true, Volume, Pitch))
	{
		LoopingSound = UGameplayStatics::SpawnSoundAtLocation(this, Wave, GetActorLocation(),
			FRotator::ZeroRotator, Volume, Pitch);
	}
}

void ASourceGameEntity::StopLoopingSoundNow()
{
	if (LoopingSound)
	{
		LoopingSound->Stop();
		LoopingSound = nullptr;
	}
}

void ASourceGameEntity::CheckBlocked()
{
	// Whatever the moving brush is currently overlapping. Reported every frame it stays blocked rather than
	// once on contact, because a door leaning on somebody keeps doing it - how often that matters is the
	// game's decision, not this one's.
	if (!Behaviour || !BrushMesh || bIsTriggerVolume)
	{
		return;
	}
	TArray<AActor*> Overlapping;
	GetOverlappingActors(Overlapping);
	for (AActor* Other : Overlapping)
	{
		if (Other && Other != this)
		{
			Behaviour->OnBlocked(FLambdaGameDll::Get().IdForEntity(Other));
			return;
		}
	}
}

void ASourceGameEntity::CancelLinearMove()
{
	if (bMoving)
	{
		bMoving = false;
		bMovingAxis = false;
		StopLoopingSoundNow();
		if (Behaviour)
		{
			Behaviour->OnMoveDone(lambda::MoveResult::Interrupted);
		}
	}
}

bool ASourceGameEntity::IsUsable() const
{
	return Behaviour ? Behaviour->IsUsable() : false;
}

void ASourceGameEntity::OnUsed(AActor* Activator)
{
	if (Behaviour)
	{
		Behaviour->OnUse(FLambdaGameDll::Get().IdForEntity(Activator));
	}
}

bool ASourceGameEntity::AcceptInput(const FString& InputName, AActor* Activator, AActor* Caller, const FString& Parameter)
{
	if (Behaviour)
	{
		const lambda::EntityId ActivatorId = FLambdaGameDll::Get().IdForEntity(Activator);
		if (Behaviour->OnInput(TCHAR_TO_ANSI(*InputName), TCHAR_TO_ANSI(*Parameter), ActivatorId))
		{
			return true;
		}
	}
	// An input the game does not implement is still one the engine might - Kill, AddOutput and the rest.
	return Super::AcceptInput(InputName, Activator, Caller, Parameter);
}
