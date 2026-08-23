#include "SourceBSPWorldActor.h"
#include "LambdaFileSystem.h"
#include "LambdaMaterialLibrary.h"
#include "LambdaSourceModule.h"
#include "LambdaSourceSettings.h"
#include "SourceCoordinates.h"
#include "SourceBrushEntity.h"
#include "SourceFuncDoorRotating.h"
#include "SourceGeometryBuilder.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SkyLight.h"
#include "Engine/SpotLight.h"
#include "Engine/TextureCube.h"
#include "Engine/World.h"
#include "GameFramework/PlayerStart.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "TextureResource.h"

ASourceBSPWorldActor::ASourceBSPWorldActor()
{
	PrimaryActorTick.bCanEverTick = false;

	WorldMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("WorldMesh"));
	RootComponent = WorldMesh;
	WorldMesh->Mobility = EComponentMobility::Static;
	WorldMesh->bUseComplexAsSimpleCollision = true;
	WorldMesh->bUseAsyncCooking = false;
	WorldMesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	WorldMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	WorldMesh->SetCastShadow(true);
}

// ---------------------------------------------------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------------------------------------------------

bool ASourceBSPWorldActor::LoadMap(const FString& MapName)
{
	FString Name = FLambdaFileSystem::NormalizeRelativePath(MapName);
	if (Name.EndsWith(TEXT(".bsp"), ESearchCase::IgnoreCase))
	{
		Name.LeftChopInline(4);
	}
	if (Name.StartsWith(TEXT("maps/"), ESearchCase::IgnoreCase))
	{
		Name.RightChopInline(5);
	}
	if (Name.IsEmpty())
	{
		UE_LOG(LogLambdaSource, Error, TEXT("LoadMap: empty map name"));
		return false;
	}
	return LoadBSPFile(FString::Printf(TEXT("maps/%s.bsp"), *Name));
}

bool ASourceBSPWorldActor::LoadBSPFile(const FString& RelativePath)
{
	const double StartTime = FPlatformTime::Seconds();
	ClearMap();

	// Read via the virtual file system so the map can come from a loose game dir OR a mounted VPK.
	TArray<uint8> BSPBytes;
	if (!FLambdaFileSystem::Get().ReadFile(RelativePath, BSPBytes))
	{
		UE_LOG(LogLambdaSource, Error, TEXT("BSP not found: '%s' (mounts: %s)"), *RelativePath,
			*FString::Join(FLambdaFileSystem::Get().GetMountDescriptions(), TEXT("; ")));
		return false;
	}

	TSharedPtr<FSourceBSPFile> NewBSP = MakeShared<FSourceBSPFile>();
	FString Error;
	if (!NewBSP->LoadFromMemory(MoveTemp(BSPBytes), &Error))
	{
		UE_LOG(LogLambdaSource, Error, TEXT("Failed to load BSP '%s': %s"), *RelativePath, *Error);
		return false;
	}

	BSP = NewBSP;
	LoadedMapName = FPaths::GetBaseFilename(RelativePath);
	BSP->LogSummary(LoadedMapName);

	MaterialLibrary = NewObject<ULambdaMaterialLibrary>(this);
	MaterialLibrary->Initialize();

	Stats = FSourceBSPLoadStats();
	Stats.NumFaces = BSP->Faces.Num();
	Stats.NumEntities = BSP->Entities.Num();

	BuildWorldGeometry();
	SpawnEntities();

	Stats.NumMaterials = MaterialLibrary->GetNumMaterials();
	Stats.NumTextures = MaterialLibrary->GetNumTextures();
	Stats.LoadTimeSeconds = (float)(FPlatformTime::Seconds() - StartTime);

	UE_LOG(LogLambdaSource, Log, TEXT("Loaded map '%s' in %.2fs: %d faces (%d rendered, %d collision-only, %d skipped, %d displacement), %d verts, %d tris, %d sections, %d materials, %d textures, %d entities, %d lights, %d player starts, %d brush entities"),
		*LoadedMapName, Stats.LoadTimeSeconds, Stats.NumFaces, Stats.NumRenderedFaces, Stats.NumCollisionOnlyFaces, Stats.NumSkippedFaces, Stats.NumDisplacementFaces,
		Stats.NumVertices, Stats.NumTriangles, Stats.NumSections, Stats.NumMaterials, Stats.NumTextures, Stats.NumEntities, Stats.NumLights, Stats.NumPlayerStarts, Stats.NumBrushEntities);
	LogUnhandledEntities();
	return true;
}

void ASourceBSPWorldActor::ClearMap()
{
	if (WorldMesh)
	{
		WorldMesh->ClearAllMeshSections();
	}
	for (const TObjectPtr<AActor>& Actor : SpawnedActors)
	{
		if (IsValid(Actor))
		{
			Actor->Destroy();
		}
	}
	SpawnedActors.Reset();
	BSP.Reset();
	LoadedMapName.Reset();
	MaterialLibrary = nullptr;
	UnhandledEntityCounts.Reset();
	Stats = FSourceBSPLoadStats();
	bSpawnedSkyLight = false;
}

TArray<const FSourceEntity*> ASourceBSPWorldActor::FindEntities(const FString& ClassName) const
{
	return BSP.IsValid() ? BSP->FindEntities(ClassName) : TArray<const FSourceEntity*>();
}

// ---------------------------------------------------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------------------------------------------------

void ASourceBSPWorldActor::BuildWorldGeometry()
{
	const float Scale = ULambdaSourceSettings::Get().UnitScale;

	// Model 0 is the world; models 1..N belong to brush entities and are built by those actors instead.
	TArray<FSourceMeshSection> Sections;
	FSourceGeometryStats GeoStats;
	SourceGeometry::BuildModel(*BSP, 0, Scale, Sections, GeoStats);
	SourceGeometry::ApplyToComponent(WorldMesh, Sections, MaterialLibrary);

	Stats.NumRenderedFaces = GeoStats.NumRenderedFaces;
	Stats.NumCollisionOnlyFaces = GeoStats.NumCollisionOnlyFaces;
	Stats.NumSkippedFaces = GeoStats.NumSkippedFaces;
	Stats.NumDisplacementFaces = GeoStats.NumDisplacementFaces;
	Stats.NumVertices = GeoStats.NumVertices;
	Stats.NumTriangles = GeoStats.NumTriangles;
	Stats.NumSections = Sections.Num();

	UE_LOG(LogLambdaSource, Verbose, TEXT("Winding: %d/%d faces used natural order"),
		GeoStats.NumNaturalWindingFaces, GeoStats.NumRenderedFaces + GeoStats.NumCollisionOnlyFaces);
}

// ---------------------------------------------------------------------------------------------------------------------
// Entities
// ---------------------------------------------------------------------------------------------------------------------

void ASourceBSPWorldActor::SpawnEntities()
{
	UWorld* World = GetWorld();
	if (!World || !BSP.IsValid())
	{
		return;
	}

	for (const FSourceEntity& Entity : BSP->Entities)
	{
		const FString& Class = Entity.ClassName;

		// Brush entities carry their geometry as "model" "*N"; they own that BSP model, not the world mesh.
		const FString ModelRef = Entity.Get(TEXT("model"));
		if (ModelRef.StartsWith(TEXT("*")))
		{
			const int32 ModelIndex = FCString::Atoi(*ModelRef.RightChop(1));
			if (ModelIndex > 0 && BSP->Models.IsValidIndex(ModelIndex))
			{
				SpawnBrushEntity(Entity, ModelIndex);
			}
			else
			{
				UE_LOG(LogLambdaSource, Warning, TEXT("Entity '%s' references missing brush model '%s'"), *Class, *ModelRef);
			}
			continue;
		}

		if (Class.Equals(TEXT("info_player_start"), ESearchCase::IgnoreCase) ||
			Class.Equals(TEXT("info_player_deathmatch"), ESearchCase::IgnoreCase) ||
			Class.Equals(TEXT("info_player_coop"), ESearchCase::IgnoreCase) ||
			Class.Equals(TEXT("info_player_terrorist"), ESearchCase::IgnoreCase) ||
			Class.Equals(TEXT("info_player_counterterrorist"), ESearchCase::IgnoreCase))
		{
			SpawnPlayerStart(Entity);
		}
		else if (Class.Equals(TEXT("light"), ESearchCase::IgnoreCase))
		{
			SpawnPointLight(Entity);
		}
		else if (Class.Equals(TEXT("light_spot"), ESearchCase::IgnoreCase))
		{
			SpawnSpotLight(Entity);
		}
		else if (Class.Equals(TEXT("light_environment"), ESearchCase::IgnoreCase))
		{
			SpawnEnvironmentLight(Entity);
		}
		else if (Class.Equals(TEXT("worldspawn"), ESearchCase::IgnoreCase))
		{
			// handled implicitly (model 0)
		}
		else
		{
			UnhandledEntityCounts.FindOrAdd(Class)++;
		}
	}

	const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();
	if (!bSpawnedSkyLight && Settings.AmbientFillIntensity > 0.0f)
	{
		SpawnAmbientFill(Settings.AmbientFillColor, Settings.AmbientFillIntensity);
	}
}

void ASourceBSPWorldActor::SpawnBrushEntity(const FSourceEntity& Entity, int32 ModelIndex)
{
	UWorld* World = GetWorld();
	const FString& Class = Entity.ClassName;

	TSubclassOf<ASourceBrushEntity> ActorClass;
	if (Class.Equals(TEXT("func_door_rotating"), ESearchCase::IgnoreCase))
	{
		ActorClass = ASourceFuncDoorRotating::StaticClass();
	}
	else
	{
		// Not implemented yet: render the brush so the map still looks right, but with no behaviour.
		ActorClass = ASourceBrushEntity::StaticClass();
		UnhandledEntityCounts.FindOrAdd(Class)++;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;
	ASourceBrushEntity* BrushActor = World->SpawnActor<ASourceBrushEntity>(ActorClass, FTransform::Identity, Params);
	if (!BrushActor)
	{
		return;
	}
	BrushActor->InitializeFromEntity(*BSP, ModelIndex, Entity, MaterialLibrary);
	SpawnedActors.Add(BrushActor);
	++Stats.NumBrushEntities;
}

void ASourceBSPWorldActor::SpawnPlayerStart(const FSourceEntity& Entity)
{
	UWorld* World = GetWorld();
	const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();

	FVector3f Origin;
	if (!Entity.GetVector(TEXT("origin"), Origin))
	{
		return;
	}
	FVector3f Angles = FVector3f::ZeroVector;
	Entity.GetVector(TEXT("angles"), Angles);

	// Source spawn origins are at the feet; UE spawns the capsule centre at the PlayerStart.
	const float HalfHeightCm = Settings.UnitsToCm(Settings.PlayerCapsuleHalfHeightUnits);
	const FVector Location = FSourceCoords::ToUE(Origin, Settings.UnitScale) + FVector(0.0, 0.0, HalfHeightCm + 2.0);
	const FRotator Rotation(0.0f, FSourceCoords::AnglesToUE(Angles).Yaw, 0.0f);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;
	APlayerStart* Start = World->SpawnActor<APlayerStart>(APlayerStart::StaticClass(), Location, Rotation, Params);
	if (Start)
	{
		Start->PlayerStartTag = FName(*Entity.ClassName);
		SpawnedActors.Add(Start);
		++Stats.NumPlayerStarts;
		UE_LOG(LogLambdaSource, Log, TEXT("PlayerStart '%s' at Source(%s) -> UE(%s) yaw=%.1f"), *Entity.ClassName, *Origin.ToString(), *Location.ToString(), Rotation.Yaw);
	}
}

bool ASourceBSPWorldActor::ParseLightColor(const FSourceEntity& Entity, const TCHAR* Key, FLinearColor& OutColor, float& OutBrightness)
{
	// "_light" "R G B [brightness]"  (colour in gamma space, brightness ~200 for a typical room light)
	TArray<float> Values;
	const FString Text = Entity.Get(Key);
	if (Text.IsEmpty() || !FSourceCoords::ParseFloats(Text, Values) || Values.Num() < 3)
	{
		OutColor = FLinearColor::White;
		OutBrightness = 200.0f;
		return false;
	}
	const FColor SRGB(
		(uint8)FMath::Clamp(FMath::RoundToInt(Values[0]), 0, 255),
		(uint8)FMath::Clamp(FMath::RoundToInt(Values[1]), 0, 255),
		(uint8)FMath::Clamp(FMath::RoundToInt(Values[2]), 0, 255));
	OutColor = FLinearColor(SRGB); // sRGB -> linear
	OutBrightness = (Values.Num() >= 4) ? Values[3] : 200.0f;
	return true;
}

void ASourceBSPWorldActor::GetLightDirectionAngles(const FSourceEntity& Entity, float& OutPitch, float& OutYaw)
{
	// vrad: yaw from "angle" (if non-zero) else angles.yaw; pitch from "pitch" (if non-zero) else angles.pitch; light pitch uses +sin(pitch) = up.
	FVector3f Angles = FVector3f::ZeroVector;
	Entity.GetVector(TEXT("angles"), Angles);
	float Pitch = Entity.GetFloat(TEXT("pitch"), 0.0f);
	if (FMath::IsNearlyZero(Pitch))
	{
		Pitch = Angles.X;
	}
	float Yaw = Angles.Y;
	const float AngleKey = Entity.GetFloat(TEXT("angle"), 0.0f);
	if (FMath::IsNearlyEqual(AngleKey, -1.0f))		// ANGLE_UP
	{
		Pitch = 90.0f;
	}
	else if (FMath::IsNearlyEqual(AngleKey, -2.0f))	// ANGLE_DOWN
	{
		Pitch = -90.0f;
	}
	else if (!FMath::IsNearlyZero(AngleKey))
	{
		Yaw = AngleKey;
	}
	OutPitch = Pitch;
	OutYaw = Yaw;
}

void ASourceBSPWorldActor::ConfigureLocalLight(ULocalLightComponent* Light, const FSourceEntity& Entity) const
{
	const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();

	FLinearColor Color;
	float Brightness;
	ParseLightColor(Entity, TEXT("_light"), Color, Brightness);

	Light->SetMobility(EComponentMobility::Movable);
	Light->SetIntensityUnits(ELightUnits::Candelas);
	Light->SetIntensity(Brightness * Settings.LightIntensityScale);
	Light->SetLightColor(Color);
	Light->SetCastShadows(Settings.bLightsCastShadows);

	float RadiusUnits = Entity.GetFloat(TEXT("_zero_percent_distance"), 0.0f);
	if (RadiusUnits <= 0.0f)
	{
		RadiusUnits = Settings.DefaultLightRadiusUnits;
	}
	Light->SetAttenuationRadius(Settings.UnitsToCm(RadiusUnits));
}

void ASourceBSPWorldActor::SpawnPointLight(const FSourceEntity& Entity)
{
	UWorld* World = GetWorld();
	FVector3f Origin;
	if (!Entity.GetVector(TEXT("origin"), Origin))
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;
	APointLight* Actor = World->SpawnActor<APointLight>(APointLight::StaticClass(), FSourceCoords::ToUE(Origin), FRotator::ZeroRotator, Params);
	if (!Actor)
	{
		return;
	}
	ConfigureLocalLight(Actor->PointLightComponent, Entity);
	SpawnedActors.Add(Actor);
	++Stats.NumLights;
	UE_LOG(LogLambdaSource, Log, TEXT("light at Source(%s): %s cd, colour %s, radius %.0f cm"), *Origin.ToString(),
		*FString::SanitizeFloat(Actor->PointLightComponent->Intensity), *Actor->PointLightComponent->GetLightColor().ToString(), Actor->PointLightComponent->AttenuationRadius);
}

void ASourceBSPWorldActor::SpawnSpotLight(const FSourceEntity& Entity)
{
	UWorld* World = GetWorld();
	FVector3f Origin;
	if (!Entity.GetVector(TEXT("origin"), Origin))
	{
		return;
	}
	float Pitch, Yaw;
	GetLightDirectionAngles(Entity, Pitch, Yaw);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;
	ASpotLight* Actor = World->SpawnActor<ASpotLight>(ASpotLight::StaticClass(), FSourceCoords::ToUE(Origin), FSourceCoords::LightAnglesToUE(Pitch, Yaw), Params);
	if (!Actor)
	{
		return;
	}
	USpotLightComponent* Light = Actor->SpotLightComponent;
	ConfigureLocalLight(Light, Entity);
	const float OuterCone = FMath::Clamp(Entity.GetFloat(TEXT("_cone"), 45.0f), 1.0f, 80.0f);
	const float InnerCone = FMath::Clamp(Entity.GetFloat(TEXT("_inner_cone"), 30.0f), 0.0f, OuterCone);
	Light->SetOuterConeAngle(OuterCone);
	Light->SetInnerConeAngle(InnerCone);
	SpawnedActors.Add(Actor);
	++Stats.NumLights;
}

void ASourceBSPWorldActor::SpawnEnvironmentLight(const FSourceEntity& Entity)
{
	UWorld* World = GetWorld();
	const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();

	float Pitch, Yaw;
	GetLightDirectionAngles(Entity, Pitch, Yaw);

	FLinearColor SunColor;
	float SunBrightness;
	ParseLightColor(Entity, TEXT("_light"), SunColor, SunBrightness);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;
	ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), FVector::ZeroVector, FSourceCoords::LightAnglesToUE(Pitch, Yaw), Params);
	if (Sun)
	{
		ULightComponent* Light = Sun->GetLightComponent();
		Light->SetMobility(EComponentMobility::Movable);
		Light->SetIntensity(SunBrightness * Settings.SunIntensityScale);
		Light->SetLightColor(SunColor);
		Light->SetCastShadows(Settings.bLightsCastShadows);
		SpawnedActors.Add(Sun);
		++Stats.NumLights;
	}

	// "_ambient" "R G B brightness" -> sky light
	FLinearColor AmbientColor;
	float AmbientBrightness;
	if (ParseLightColor(Entity, TEXT("_ambient"), AmbientColor, AmbientBrightness))
	{
		const float Intensity = FMath::Max(Settings.AmbientFillIntensity, 0.1f) * (AmbientBrightness / 200.0f);
		SpawnAmbientFill(AmbientColor, Intensity);
	}
}

namespace
{
	/** A tiny uniform-colour cubemap so a sky light can provide constant ambient light without any sky. */
	UTextureCube* CreateUniformCubemap(const FColor& Color)
	{
		UTextureCube* Cube = UTextureCube::CreateTransient(8, 8, PF_B8G8R8A8, NAME_None);
		if (!Cube)
		{
			return nullptr;
		}
		FTexture2DMipMap& Mip = Cube->GetPlatformData()->Mips[0];
		const int64 NumBytes = Mip.BulkData.GetBulkDataSize();
		uint8* Dst = (uint8*)Mip.BulkData.Lock(LOCK_READ_WRITE);
		for (int64 i = 0; i + 3 < NumBytes; i += 4)
		{
			Dst[i + 0] = Color.B;
			Dst[i + 1] = Color.G;
			Dst[i + 2] = Color.R;
			Dst[i + 3] = 255;
		}
		Mip.BulkData.Unlock();
		Cube->SRGB = true;
		Cube->NeverStream = true;
		Cube->UpdateResource();
		return Cube;
	}
}

void ASourceBSPWorldActor::SpawnAmbientFill(const FLinearColor& Color, float Intensity)
{
	UWorld* World = GetWorld();
	UTextureCube* Cube = CreateUniformCubemap(FColor::White);
	if (!Cube)
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Could not create ambient cubemap"));
		return;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;
	ASkyLight* SkyActor = World->SpawnActor<ASkyLight>(ASkyLight::StaticClass(), FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator, Params);
	if (!SkyActor)
	{
		return;
	}
	USkyLightComponent* Sky = SkyActor->GetLightComponent();
	Sky->SetMobility(EComponentMobility::Movable);
	Sky->bRealTimeCapture = false;
	Sky->SourceType = SLS_SpecifiedCubemap;
	Sky->bLowerHemisphereIsBlack = false;
	Sky->SetCubemap(Cube);
	Sky->SetLightColor(Color);
	Sky->SetIntensity(Intensity);
	Sky->SetCastShadows(false);
	Sky->MarkRenderStateDirty();
	Sky->RecaptureSky();

	SpawnedActors.Add(SkyActor);
	bSpawnedSkyLight = true;
	UE_LOG(LogLambdaSource, Log, TEXT("Ambient sky light: intensity %.2f colour %s"), Intensity, *Color.ToString());
}

void ASourceBSPWorldActor::LogUnhandledEntities() const
{
	for (const TPair<FString, int32>& Pair : UnhandledEntityCounts)
	{
		UE_LOG(LogLambdaSource, Log, TEXT("Unhandled entity class '%s' x%d"), *Pair.Key, Pair.Value);
	}
}
