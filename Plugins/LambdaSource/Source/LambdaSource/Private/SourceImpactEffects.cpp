#include "SourceImpactEffects.h"
#include "LambdaMaterialLibrary.h"
#include "LambdaSoundLibrary.h"
#include "LambdaSourceModule.h"
#include "LambdaSourceSettings.h"
#include "SourceDecalScript.h"
#include "SourceGeometryBuilder.h"
#include "SourceSurfaceProps.h"
#include "Components/DecalComponent.h"
#include "Engine/HitResult.h"
#include "Kismet/GameplayStatics.h"

namespace SourceImpact
{

bool ResolveSurface(const FHitResult& Hit, ULambdaMaterialLibrary* Materials, FSurfaceHitInfo& OutInfo)
{
	OutInfo = FSurfaceHitInfo();

	const USourceBrushMeshComponent* Mesh = Cast<USourceBrushMeshComponent>(Hit.GetComponent());
	if (Mesh && Hit.FaceIndex != INDEX_NONE)
	{
		OutInfo.MaterialName = Mesh->GetMaterialNameForFaceIndex(Hit.FaceIndex);
	}

	if (Materials && !OutInfo.MaterialName.IsEmpty())
	{
		OutInfo.SurfaceProp = Materials->GetSurfaceProp(OutInfo.MaterialName);
	}

	FSourceSurfaceProps& Props = FSourceSurfaceProps::Get();
	Props.Initialize();
	OutInfo.GameMaterial = Props.GetGameMaterial(OutInfo.SurfaceProp);
	if (const FSourceSurfaceProp* Prop = Props.Find(OutInfo.SurfaceProp))
	{
		OutInfo.BulletImpactSound = Prop->BulletImpactSound;
	}
	else if (const FSourceSurfaceProp* Default = Props.Find(TEXT("default")))
	{
		OutInfo.BulletImpactSound = Default->BulletImpactSound;
	}
	return !OutInfo.MaterialName.IsEmpty();
}

void PlayImpact(const FHitResult& Hit, ULambdaMaterialLibrary* Materials, UObject* SoundOuter)
{
	UWorld* World = Hit.GetComponent() ? Hit.GetComponent()->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}

	FSurfaceHitInfo Info;
	ResolveSurface(Hit, Materials, Info);

	const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();

	// ---- Decal ----
	FSourceDecalScript& Decals = FSourceDecalScript::Get();
	Decals.Initialize();
	const FString DecalName = Decals.GetImpactDecalMaterial(Info.GameMaterial);
	if (!DecalName.IsEmpty() && Materials)
	{
		float SizeUnits = 6.4f;
		if (UMaterialInterface* DecalMaterial = Materials->GetDecalMaterial(DecalName, SizeUnits))
		{
			// UDecalComponent projects along its +X axis, so face it down the surface normal. Source picks a
			// random roll for each decal; without it repeated hits stamp an identical image.
			const float SizeCm = SizeUnits * Settings.UnitScale;
			FRotator Rotation = (-Hit.ImpactNormal).Rotation();
			Rotation.Roll = FMath::FRandRange(0.0f, 360.0f);

			// Attaching to the component that was hit means decals ride along with brush entities such as doors.
			UDecalComponent* Decal = UGameplayStatics::SpawnDecalAttached(DecalMaterial,
				FVector(SizeCm, SizeCm * 0.5f, SizeCm * 0.5f), Hit.GetComponent(), NAME_None,
				Hit.ImpactPoint, Rotation, EAttachLocation::KeepWorldPosition, Settings.DecalLifetime);
			if (Decal)
			{
				Decal->SetFadeScreenSize(0.0f);	// keep small bullet holes visible at a distance
			}
		}
	}

	// ---- Sound ----
	if (!Info.BulletImpactSound.IsEmpty() && SoundOuter)
	{
		float Volume = 1.0f, Pitch = 1.0f;
		if (ULambdaSoundWave* Wave = FLambdaSoundCache::Get().CreateWaveResolved(SoundOuter, Info.BulletImpactSound, false, Volume, Pitch))
		{
			UGameplayStatics::SpawnSoundAtLocation(SoundOuter, Wave, Hit.ImpactPoint, FRotator::ZeroRotator, Volume, Pitch);
		}
	}

	UE_LOG(LogLambdaSource, Verbose, TEXT("Impact on '%s': surfaceprop '%s' gamematerial '%c' -> decal '%s'"),
		*Info.MaterialName, *Info.SurfaceProp, Info.GameMaterial, *DecalName);
}

}	// namespace SourceImpact
