#include "Rendering/SourceLightFlash.h"

#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"

ASourceLightFlash::ASourceLightFlash()
{
	PrimaryActorTick.bCanEverTick = true;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));

	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetMobility(EComponentMobility::Movable);
	Light->SetIntensityUnits(ELightUnits::Candelas);
	// Nothing in the flash is meant to be seen as a source: the particle effect is the fireball.
	Light->SetSourceRadius(0.0f);
	Light->bUseInverseSquaredFalloff = true;
}

ASourceLightFlash* ASourceLightFlash::Create(UWorld* World, const FVector& Location, const FLinearColor& Color,
	float IntensityCandelas, float RadiusCm, float InLifeSeconds, bool bCastShadows)
{
	if (!World || InLifeSeconds <= 0.0f)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;
	ASourceLightFlash* Flash = World->SpawnActor<ASourceLightFlash>(ASourceLightFlash::StaticClass(), Location, FRotator::ZeroRotator, Params);
	if (!Flash)
	{
		return nullptr;
	}
	Flash->StartTime = World->GetTimeSeconds();
	Flash->LifeSeconds = InLifeSeconds;
	Flash->PeakIntensity = IntensityCandelas;
	Flash->PeakRadius = RadiusCm;
	Flash->Light->SetLightColor(Color);
	Flash->Light->SetIntensity(IntensityCandelas);
	Flash->Light->SetAttenuationRadius(RadiusCm);
	Flash->Light->SetCastShadows(bCastShadows);
	return Flash;
}

void ASourceLightFlash::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const UWorld* World = GetWorld();
	if (!World || !Light)
	{
		return;
	}
	const float T = (World->GetTimeSeconds() - StartTime) / LifeSeconds;
	if (T >= 1.0f)
	{
		Destroy();
		return;
	}
	// Bright at once, then gone: the fall-off is steeper than linear so the flash reads as a flash and not
	// as a lamp being dimmed, and the reach shrinks with it (dlight_t::decay).
	const float Fade = FMath::Square(1.0f - T);
	Light->SetIntensity(PeakIntensity * Fade);
	Light->SetAttenuationRadius(PeakRadius * (0.5f + 0.5f * (1.0f - T)));
}
