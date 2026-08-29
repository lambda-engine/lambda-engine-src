#include "Entities/SourceGamePointEntity.h"

#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Core/LambdaSourceModule.h"
#include "Game/LambdaGameDll.h"

ASourceGamePointEntity::ASourceGamePointEntity()
{
	PrimaryActorTick.bCanEverTick = true;
	// A pattern has to keep running while the player is elsewhere: a corridor whose strobe only starts once
	// you can see it reads as the light reacting to you.
	PrimaryActorTick.bStartWithTickEnabled = true;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

ULocalLightComponent* ASourceGamePointEntity::CreateLightComponent(const FSourceEntity& InEntity)
{
	if (InEntity.ClassName.Equals(TEXT("light_spot"), ESearchCase::IgnoreCase))
	{
		LightComponent = NewObject<USpotLightComponent>(this, TEXT("SpotLight"));
	}
	else
	{
		LightComponent = NewObject<UPointLightComponent>(this, TEXT("PointLight"));
	}
	LightComponent->SetupAttachment(GetRootComponent());
	LightComponent->RegisterComponent();
	return LightComponent;
}

void ASourceGamePointEntity::InitializeGameEntity(const FSourceEntity& InEntity, ASourceBSPWorldActor* InWorldActor,
	ULocalLightComponent* ConfiguredLight)
{
	InitializeEntity(InEntity, InWorldActor);

	// Captured after the caller has set colour and intensity from the map, because the game's pattern scales
	// whatever the light is worth at full brightness.
	LightComponent = ConfiguredLight;
	BaseIntensity = LightComponent ? LightComponent->Intensity : 0.0f;

	Behaviour = FLambdaGameDll::Get().CreateEntity(InEntity.ClassName, this, GameId);
	if (!Behaviour)
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("%s: the game module claimed this class but made no entity for it"),
			*InEntity.ClassName);
		return;
	}
	Behaviour->Spawn();
}

void ASourceGamePointEntity::EndPlay(const EEndPlayReason::Type Reason)
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

void ASourceGamePointEntity::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (Behaviour)
	{
		Behaviour->Think(DeltaSeconds);
	}
}

bool ASourceGamePointEntity::AcceptInput(const FString& InputName, AActor* Activator, AActor* Caller, const FString& Parameter)
{
	if (Behaviour)
	{
		const lambda::EntityId ActivatorId = FLambdaGameDll::Get().IdForEntity(Activator);
		if (Behaviour->OnInput(TCHAR_TO_ANSI(*InputName), TCHAR_TO_ANSI(*Parameter), ActivatorId))
		{
			return true;
		}
	}
	return Super::AcceptInput(InputName, Activator, Caller, Parameter);
}

/** Declared in SourceLight.cpp; the same light_debug flag reports both implementations. */
extern bool GLightDebug;

void ASourceGamePointEntity::SetLightScale(float Scale)
{
	if (!LightComponent)
	{
		return;
	}
	if (GLightDebug)
	{
		UE_LOG(LogLambdaSource, Display, TEXT("light_debug t=%.2f '%s' [game module] scale=%.3f intensity=%.1f"),
			GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f, *GetTargetName(), Scale, BaseIntensity * Scale);
	}
	LightComponent->SetIntensity(BaseIntensity * Scale);
	// Switched off rather than left burning at zero: a light with no output still costs a shadow-casting pass
	// every frame, and a corridor of flickering lights is mostly off at any moment.
	LightComponent->SetVisibility(Scale > 0.0f);
}
