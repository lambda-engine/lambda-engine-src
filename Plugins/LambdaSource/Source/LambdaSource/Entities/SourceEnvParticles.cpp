#include "Entities/SourceEnvParticles.h"
#include "Particles/SourceParticleSystem.h"
#include "Materials/LambdaMaterialLibrary.h"
#include "World/SourceBSPWorldActor.h"
#include "Core/LambdaSourceModule.h"
#include "Engine/World.h"
#include "Components/SceneComponent.h"

ASourceEnvParticles::ASourceEnvParticles()
{
	PrimaryActorTick.bCanEverTick = true;
	// Something has to hold the origin and angles the map gave it; without a root the spawn transform is lost.
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

void ASourceEnvParticles::InitializeEntity(const FSourceEntity& InEntity, ASourceBSPWorldActor* InWorldActor)
{
	Super::InitializeEntity(InEntity, InWorldActor);
	EffectName = Entity.Get(TEXT("effect_name"));
	bStartActive = Entity.GetInt(TEXT("start_active"), 0) != 0;
	FVector3f ReadAngles;
	if (Entity.GetVector(TEXT("angles"), ReadAngles))
	{
		Angles = ReadAngles;
	}

	// cpoint1..63 are read in order and stop at the first blank, as CParticleSystem::ReadControlPointEnts does.
	ControlPointTargets.SetNum(64);
	for (int32 i = 1; i < 64; ++i)
	{
		const FString Target = Entity.Get(FString::Printf(TEXT("cpoint%d"), i));
		if (Target.IsEmpty())
		{
			break;
		}
		ControlPointTargets[i] = Target;
	}

	if (EffectName.IsEmpty())
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("%s '%s' has no effect_name"), *Entity.ClassName, *TargetName);
	}
}

void ASourceEnvParticles::ResolveControlPoints()
{
	ControlPointActors.SetNum(64);
	ASourceBSPWorldActor* Map = WorldActor.Get();
	if (!Map)
	{
		return;
	}
	for (int32 i = 1; i < 64; ++i)
	{
		if (ControlPointTargets[i].IsEmpty())
		{
			break;
		}
		TArray<ASourceEntity*> Found;
		Map->ResolveTargets(ControlPointTargets[i], this, this, Found);
		if (Found.Num() > 0 && Found[0])
		{
			ControlPointActors[i] = Found[0];
		}
		else
		{
			UE_LOG(LogLambdaSource, Warning, TEXT("%s '%s': cpoint%d '%s' names nothing in the map"),
				*Entity.ClassName, *TargetName, i, *ControlPointTargets[i]);
		}
	}
}

void ASourceEnvParticles::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// The targets can only be looked up once the whole map has spawned, which the first tick is the first
	// moment after.
	if (!bControlPointsResolved)
	{
		bControlPointsResolved = true;
		ResolveControlPoints();
		if (bStartActive)
		{
			StartEffect();
		}
	}

	if (System)
	{
		for (int32 i = 1; i < ControlPointActors.Num(); ++i)
		{
			if (AActor* Target = ControlPointActors[i].Get())
			{
				System->SetControlPointLocation(i, Target->GetActorLocation());
			}
		}
	}
}

void ASourceEnvParticles::StartEffect()
{
	if (EffectName.IsEmpty())
	{
		return;
	}
	if (!System)
	{
		System = ASourceParticleSystem::Create(GetWorld(), EffectName, GetActorLocation(), Angles, MaterialLibrary, /*bOneShot=*/ false);
		if (!System)
		{
			return;
		}
		System->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
		for (int32 i = 1; i < ControlPointActors.Num(); ++i)
		{
			if (AActor* Target = ControlPointActors[i].Get())
			{
				System->SetControlPointLocation(i, Target->GetActorLocation());
			}
		}
	}
	else
	{
		System->StartEffect();
	}
}

void ASourceEnvParticles::StopEffect(bool bDestroyImmediately)
{
	if (!System)
	{
		return;
	}
	if (bDestroyImmediately)
	{
		System->StopAndDestroy();
		System = nullptr;
	}
	else
	{
		System->StopEffect(false);
	}
}

bool ASourceEnvParticles::IsEffectActive() const
{
	return System && System->IsActive() && !System->IsFinished();
}

bool ASourceEnvParticles::AcceptInput(const FString& InputName, AActor* Activator, AActor* Caller, const FString& Parameter)
{
	if (InputName.Equals(TEXT("Start"), ESearchCase::IgnoreCase))
	{
		StartEffect();
		return true;
	}
	if (InputName.Equals(TEXT("Stop"), ESearchCase::IgnoreCase))
	{
		StopEffect(false);
		return true;
	}
	if (InputName.Equals(TEXT("DestroyImmediately"), ESearchCase::IgnoreCase))
	{
		StopEffect(true);
		return true;
	}
	return Super::AcceptInput(InputName, Activator, Caller, Parameter);
}

void ASourceEnvParticles::EndPlay(const EEndPlayReason::Type Reason)
{
	if (System && IsValid(System))
	{
		System->Destroy();
	}
	System = nullptr;
	Super::EndPlay(Reason);
}
