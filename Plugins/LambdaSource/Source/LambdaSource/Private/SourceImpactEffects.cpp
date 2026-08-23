#include "SourceImpactEffects.h"
#include "LambdaMaterialLibrary.h"
#include "LambdaSoundLibrary.h"
#include "LambdaSourceModule.h"
#include "LambdaSourceSettings.h"
#include "SourceDecalScript.h"
#include "SourceGeometryBuilder.h"
#include "SourceSurfaceProps.h"
#include "Materials/MaterialInterface.h"
#include "SourceParticleEffect.h"
#include "SourceNPCBase.h"
#include "SourcePropPhysics.h"
#include "SourceRagdoll.h"
#include "SourceStudioModelComponent.h"
#include "Components/DecalComponent.h"
#include "Engine/HitResult.h"
#include "Kismet/GameplayStatics.h"


namespace
{
	/** effect_color_tables.h bloodcolors[]. */
	FLinearColor BloodColorRGB(ESourceBloodColor Color)
	{
		switch (Color)
		{
		case ESourceBloodColor::Red:    return FLinearColor(72 / 255.0f, 0.0f, 0.0f);
		case ESourceBloodColor::Yellow: return FLinearColor(195 / 255.0f, 195 / 255.0f, 0.0f);
		case ESourceBloodColor::Green:  return FLinearColor(195 / 255.0f, 195 / 255.0f, 0.0f);
		case ESourceBloodColor::Mech:   return FLinearColor(20 / 255.0f, 20 / 255.0f, 20 / 255.0f);
		case ESourceBloodColor::Zombie: return FLinearColor(72 / 255.0f, 0.0f, 0.0f);	// blood_impact_zombie_01 is a red spray
		default:                        return FLinearColor(1.0f, 0.0f, 1.0f);	// "a ridiculous default color"
		}
	}

	FVector RandomVector(float Min, float Max)
	{
		return FVector(FMath::FRandRange(Min, Max), FMath::FRandRange(Min, Max), FMath::FRandRange(Min, Max));
	}

}

namespace SourceImpact
{
	void SpawnDecal(const FHitResult& Hit, ULambdaMaterialLibrary* Materials, const FString& DecalName)
	{
		if (DecalName.IsEmpty() || !Materials || !Hit.GetComponent())
		{
			return;
		}
		const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();
		float SizeUnits = 6.4f;
		UMaterialInterface* DecalMaterial = Materials->GetDecalMaterial(DecalName, SizeUnits);
		if (!DecalMaterial)
		{
			return;
		}
		const float Variance = Materials->GetDecalSizeVariance(DecalName);
		const float SizeCm = FMath::Max(0.5f, SizeUnits + FMath::FRandRange(-Variance, Variance)) * Settings.UnitScale;
		FRotator Rotation = (-Hit.ImpactNormal).Rotation();
		Rotation.Roll = FMath::FRandRange(0.0f, 360.0f);
		UDecalComponent* Decal = UGameplayStatics::SpawnDecalAttached(DecalMaterial,
			FVector(SizeCm, SizeCm * 0.5f, SizeCm * 0.5f), Hit.GetComponent(), NAME_None,
			Hit.ImpactPoint, Rotation, EAttachLocation::KeepWorldPosition, Settings.DecalLifetime);
		if (Decal)
		{
			Decal->SetFadeScreenSize(0.0f);
		}
		UE_LOG(LogLambdaSource, Verbose, TEXT("SpawnDecal '%s' size %.1f cm at %s -> %s"), *DecalName, SizeCm,
			*Hit.ImpactPoint.ToString(), Decal ? TEXT("ok") : TEXT("FAILED"));
	}
}

namespace SourceImpact
{

bool TraceBullet(UWorld* World, const FVector& Start, const FVector& End, FCollisionQueryParams Params, FHitResult& OutHit, int32& OutHitGroup)
{
	OutHitGroup = SourceHitGroup::HITGROUP_GENERIC;
	if (!World)
	{
		return false;
	}
	for (int32 Pass = 0; Pass < 8; ++Pass)
	{
		if (!World->LineTraceSingleByChannel(OutHit, Start, End, ECC_Visibility, Params))
		{
			return false;
		}
		ASourceNPCBase* NPC = Cast<ASourceNPCBase>(OutHit.GetActor());
		if (NPC && NPC->HasHitboxes())
		{
			FSourceHitboxHit Box;
			if (NPC->TraceHitboxes(Start, End, Box))
			{
				OutHit.ImpactPoint = Box.Point;
				OutHit.Location = Box.Point;
				OutHit.Distance = Box.Distance;
				OutHitGroup = Box.Group;
				return true;
			}
			// the hull was in the way but no hitbox was: the bullet passes this NPC by
			Params.AddIgnoredActor(NPC);
			continue;
		}
		return true;
	}
	return false;
}

void SpawnBlood(UWorld* World, ULambdaMaterialLibrary* Materials, const FVector& Origin, const FVector& Normal, ESourceBloodColor Color)
{
	// FX_BloodBulletImpact (game/client/fx_blood.cpp). Source scales the colour by the world light at the point;
	// we leave that to the renderer. Units: sizes and speeds are Hammer units, converted here.
	if (!World || Color == ESourceBloodColor::DontBleed)
	{
		return;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const FLinearColor BaseColor = BloodColorRGB(Color);

	ASourceParticleEffect* Emitter = ASourceParticleEffect::Create(World, Origin, Materials);	// "bloodgore"
	if (!Emitter)
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("SpawnBlood: could not spawn the particle emitter"));
		return;
	}
	UE_LOG(LogLambdaSource, Verbose, TEXT("SpawnBlood at %s normal %s colour %d"), *Origin.ToString(), *Normal.ToString(), (int32)Color);
	Emitter->SetGravity(200.0f * Scale);
	const int32 Core = Emitter->AddMaterial(TEXT("effects/blood_core"));
	const int32 Gore = Emitter->AddMaterial(TEXT("effects/blood_gore"));

	const FVector Dir = Normal * RandomVector(-0.5f, 0.5f);	// component-wise, as Vector * Vector is in Source
	const FVector Offset = Origin + 2.0f * Scale * Normal;

	auto Ramp = [&BaseColor]()
	{
		const float ColorRamp = FMath::FRandRange(0.75f, 2.0f);
		return FLinearColor(FMath::Min(1.0f, BaseColor.R * ColorRamp), FMath::Min(1.0f, BaseColor.G * ColorRamp), FMath::Min(1.0f, BaseColor.B * ColorRamp));
	};

	{
		FSourceSimpleParticle P;
		P.MaterialIndex = Core;
		P.Position = Offset;
		P.DieTime = FMath::FRandRange(0.25f, 0.5f);
		P.Velocity = Dir * FMath::FRandRange(16.0f, 32.0f) * Scale;
		P.Velocity.Z -= FMath::FRandRange(8.0f, 16.0f) * Scale;
		P.Color = Ramp();
		P.StartSize = FMath::RandRange(2, 4) * Scale;
		P.EndSize = P.StartSize * 8.0f;
		P.StartAlpha = 1.0f;
		P.EndAlpha = 0.0f;
		P.Roll = FMath::DegreesToRadians(FMath::RandRange(0, 360));
		Emitter->AddParticle(P);
	}
	for (int32 i = 0; i < 4; ++i)
	{
		FSourceSimpleParticle P;
		P.MaterialIndex = Gore;
		P.Position = Offset;
		P.DieTime = FMath::FRandRange(0.5f, 0.75f);
		P.Velocity = Dir * FMath::FRandRange(16.0f, 32.0f) * (i + 1) * Scale;
		P.Velocity.Z -= FMath::FRandRange(32.0f, 64.0f) * (i + 1) * Scale;
		P.Color = Ramp();
		P.StartSize = FMath::RandRange(2, 4) * Scale;
		P.EndSize = P.StartSize * 4.0f;
		P.StartAlpha = 1.0f;
		P.EndAlpha = 0.0f;
		P.Roll = FMath::DegreesToRadians(FMath::RandRange(0, 360));
		Emitter->AddParticle(P);
	}

	// The drops are a CTrailParticles emitter in Source (Setup: count 10, speed 100-400, gravity 400); trails are not
	// ported, so they are plain sprites with the same spread, speed and gravity.
	ASourceParticleEffect* Drops = ASourceParticleEffect::Create(World, Origin, Materials);	// "blooddrops"
	if (Drops)
	{
		Drops->SetGravity(400.0f * Scale);
		const int32 Drop = Drops->AddMaterial(TEXT("effects/blood_drop"));
		for (int32 i = 0; i < 10; ++i)
		{
			FSourceSimpleParticle P;
			P.MaterialIndex = Drop;
			P.Position = Origin;
			P.DieTime = FMath::FRandRange(0.2f, 0.5f);
			P.Velocity = (Normal + RandomVector(-0.5f, 0.5f)).GetSafeNormal() * FMath::FRandRange(100.0f, 400.0f) * Scale;
			P.Color = BaseColor;
			P.StartSize = FMath::FRandRange(1.0f, 2.0f) * Scale;
			P.EndSize = P.StartSize;
			P.StartAlpha = 1.0f;
			P.EndAlpha = 0.0f;
			Drops->AddParticle(P);
		}
	}
}

void TraceBleed(UWorld* World, ULambdaMaterialLibrary* Materials, const FHitResult& Wound, const FVector& ShotDirection,
	float Damage, ESourceBloodColor Color, const TArray<const AActor*>& Ignore)
{
	// CBaseEntity::TraceBleed
	if (!World || Color == ESourceBloodColor::DontBleed || Color == ESourceBloodColor::Mech || Damage == 0.0f)
	{
		return;
	}
	float FlNoise;
	int32 CCount;
	if (Damage < 10.0f)      { FlNoise = 0.1f; CCount = 1; }
	else if (Damage < 25.0f) { FlNoise = 0.2f; CCount = 2; }
	else                     { FlNoise = 0.3f; CCount = 4; }

	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const float FlTraceDist = 172.0f * Scale;

	FSourceDecalScript& Decals = FSourceDecalScript::Get();
	Decals.Initialize();
	// UTIL_BloodDecalTrace: red blood is "Blood", everything else "YellowBlood".
	const TCHAR* Group = Color == ESourceBloodColor::Red ? TEXT("Blood") : TEXT("YellowBlood");

	FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceBleed), /*bTraceComplex=*/ true);
	Params.bReturnFaceIndex = true;
	for (const AActor* A : Ignore)
	{
		if (A) { Params.AddIgnoredActor(A); }
	}

	for (int32 i = 0; i < CCount; ++i)
	{
		// trace in the direction the shot is going, with a little noise
		FVector VecTraceDir = ShotDirection;
		VecTraceDir.X += FMath::FRandRange(-FlNoise, FlNoise);
		VecTraceDir.Y += FMath::FRandRange(-FlNoise, FlNoise);
		VecTraceDir.Z += FMath::FRandRange(-FlNoise, FlNoise);

		FHitResult Bloodtr;
		if (World->LineTraceSingleByChannel(Bloodtr, Wound.ImpactPoint, Wound.ImpactPoint + VecTraceDir * FlTraceDist, ECC_Visibility, Params))
		{
			const FString DecalName = Decals.PickDecalMaterial(Group);
			UE_LOG(LogLambdaSource, Verbose, TEXT("TraceBleed: hit '%s' at %s -> '%s' decal '%s'"),
				*GetNameSafe(Bloodtr.GetActor()), *Bloodtr.ImpactPoint.ToString(), Group, *DecalName);
			SpawnDecal(Bloodtr, Materials, DecalName);
		}
		else
		{
			UE_LOG(LogLambdaSource, Verbose, TEXT("TraceBleed: trace %d hit nothing"), i);
		}
	}
}

}	// namespace SourceImpact

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
	else if (const ASourceNPCBase* NPC = Cast<ASourceNPCBase>(Hit.GetActor()))
	{
		// A studio model carries its $surfaceprop in its header (a headcrab is "alienflesh"), which is what
		// Source's bullet code reads off the hit entity's model.
		OutInfo.SurfaceProp = NPC->GetSurfaceProp();
		OutInfo.MaterialName = NPC->GetClassName();
	}
	else if (const ASourcePropPhysics* Prop = Cast<ASourcePropPhysics>(Hit.GetActor()))
	{
		// A physics prop's material comes from its collision model, the way Source reads it off the vphysics
		// object the bullet struck.
		OutInfo.SurfaceProp = Prop->GetSurfaceProp();
		OutInfo.MaterialName = Prop->GetClassName_Lambda();
	}
	else if (const ASourceRagdoll* Ragdoll = Cast<ASourceRagdoll>(Hit.GetActor()))
	{
		// A ragdoll's physics objects carry the .phy solid's surfaceprop (vphysics material of the hit object).
		OutInfo.SurfaceProp = Ragdoll->GetSurfaceProp();
		OutInfo.MaterialName = TEXT("ragdoll");
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

void PlayImpact(const FHitResult& Hit, ULambdaMaterialLibrary* Materials, UObject* SoundOuter,
	const FVector& ShotDirection, float Damage)
{
	UWorld* World = Hit.GetComponent() ? Hit.GetComponent()->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}

	FSurfaceHitInfo Info;
	ResolveSurface(Hit, Materials, Info);

	// A living thing bleeds rather than taking a bullet hole: CBaseCombatCharacter::TraceAttack spawns blood at
	// the wound and TraceBleed puts decals on whatever is behind it (baseentity_shared.cpp). A ragdoll does the
	// same (CRagdollProp::TraceAttack), so corpses keep bleeding when shot.
	const ASourceNPCBase* NPC = Cast<ASourceNPCBase>(Hit.GetActor());
	const ASourceRagdoll* Ragdoll = Cast<ASourceRagdoll>(Hit.GetActor());
	if (NPC || Ragdoll)
	{
		const ESourceBloodColor Color = NPC ? NPC->GetBloodColor() : Ragdoll->GetBloodColor();
		SpawnBlood(World, Materials, Hit.ImpactPoint, -ShotDirection, Color);
		TArray<const AActor*> Ignore;
		Ignore.Add(Hit.GetActor());
		if (const AActor* Shooter = Cast<AActor>(SoundOuter))
		{
			Ignore.Add(Shooter);
			Ignore.Add(Shooter->GetOwner());
		}
		TraceBleed(World, Materials, Hit, ShotDirection, Damage, Color, Ignore);
	}
	else
	{
		// ---- Decal ----
		FSourceDecalScript& Decals = FSourceDecalScript::Get();
		Decals.Initialize();
		// UDecalComponent projects along its +X axis, so SpawnDecalAt faces it down the surface normal with a random
		// roll (Source's) and Source 2's DecalSizeVariance, attached to what was hit so it rides along with doors.
		SpawnDecal(Hit, Materials, Decals.GetImpactDecalMaterial(Info.GameMaterial));
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

	UE_LOG(LogLambdaSource, Verbose, TEXT("Impact on '%s': surfaceprop '%s' gamematerial '%c'"),
		*Info.MaterialName, *Info.SurfaceProp, Info.GameMaterial);
}

void Precache(ULambdaMaterialLibrary* Materials, UObject* SoundOuter)
{
	const double Start = FPlatformTime::Seconds();

	FSourceSurfaceProps& Props = FSourceSurfaceProps::Get();
	FSourceDecalScript& Decals = FSourceDecalScript::Get();
	Props.Initialize();
	Decals.Initialize();

	int32 NumDecals = 0, NumSounds = 0;
	if (Materials)
	{
		// Every decal in every group, as CDecalEmitterSystem precaches them all rather than guessing which
		// surfaces the player will shoot.
		TArray<FString> DecalNames;
		Decals.GetAllDecalMaterials(DecalNames);
		for (const FString& DecalName : DecalNames)
		{
			float Size = 0.0f;
			if (Materials->GetDecalMaterial(DecalName, Size))
			{
				++NumDecals;
			}
		}

		// The bullet impact sound of every surface the loaded materials declare.
		if (SoundOuter)
		{
			TArray<FString> MaterialNames;
			Materials->GetMaterialNames(MaterialNames);
			TSet<FString> Done;
			for (const FString& MaterialName : MaterialNames)
			{
				const FString SurfaceProp = Materials->GetSurfaceProp(MaterialName);
				const FSourceSurfaceProp* Prop = Props.Find(SurfaceProp);
				if (!Prop) { Prop = Props.Find(TEXT("default")); }
				if (!Prop || Prop->BulletImpactSound.IsEmpty() || Done.Contains(Prop->BulletImpactSound))
				{
					continue;
				}
				Done.Add(Prop->BulletImpactSound);
				float Volume, Pitch;
				if (FLambdaSoundCache::Get().CreateWaveResolved(SoundOuter, Prop->BulletImpactSound, false, Volume, Pitch))
				{
					++NumSounds;
				}
			}
		}
	}

	// The blood sprites (fx_blood.cpp's CLIENTEFFECT_MATERIAL list).
	if (Materials)
	{
		static const TCHAR* BloodSprites[] = { TEXT("effects/blood_core"), TEXT("effects/blood_gore"), TEXT("effects/blood_drop") };
		for (const TCHAR* Sprite : BloodSprites)
		{
			Materials->GetSpriteMaterial(Sprite);
		}
	}

	UE_LOG(LogLambdaSource, Log, TEXT("Precached %d impact decals and %d impact sounds in %.0f ms"),
		NumDecals, NumSounds, (FPlatformTime::Seconds() - Start) * 1000.0);
}

}	// namespace SourceImpact
