#include "SourceBSPWorldActor.h"
#include "SourceImpactEffects.h"
#include "SourceNPCHeadcrab.h"
#include "SourceNPCBarnacle.h"
#include "SourceNPCZombie.h"
#include "SourceItem.h"
#include "SourcePropData.h"
#include "SourcePropPhysics.h"
#include "LambdaFileSystem.h"
#include "LambdaLoadProgress.h"
#include "SourceSkeletalMesh.h"
#include "LambdaMaterialLibrary.h"
#include "LambdaSourceModule.h"
#include "LambdaSourceSettings.h"
#include "SourceCoordinates.h"
#include "SourceBrushEntity.h"
#include "SourceFuncDoorRotating.h"
#include "SourceFuncButton.h"
#include "SourcePointTemplate.h"
#include "SourceEntity.h"
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
	PrimaryActorTick.bCanEverTick = true;

	WorldMesh = CreateDefaultSubobject<USourceBrushMeshComponent>(TEXT("WorldMesh"));
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

	// Everything below reports to the loading screen as it goes, the way Source's progress points drive its
	// loading dialog. The screen is drawn on another thread, so it keeps moving while this blocks.
	FLambdaLoadProgress::Begin(FPaths::GetBaseFilename(RelativePath));
	FLambdaLoadProgress::SetStage(ELambdaLoadStage::ReadingMap);

	// Read via the virtual file system so the map can come from a loose game dir OR a mounted VPK.
	TArray<uint8> BSPBytes;
	if (!FLambdaFileSystem::Get().ReadFile(RelativePath, BSPBytes))
	{
		UE_LOG(LogLambdaSource, Error, TEXT("BSP not found: '%s' (mounts: %s)"), *RelativePath,
			*FString::Join(FLambdaFileSystem::Get().GetMountDescriptions(), TEXT("; ")));
		FLambdaLoadProgress::End();
		return false;
	}

	TSharedPtr<FSourceBSPFile> NewBSP = MakeShared<FSourceBSPFile>();
	FString Error;
	if (!NewBSP->LoadFromMemory(MoveTemp(BSPBytes), &Error))
	{
		UE_LOG(LogLambdaSource, Error, TEXT("Failed to load BSP '%s': %s"), *RelativePath, *Error);
		FLambdaLoadProgress::End();
		return false;
	}

	BSP = NewBSP;
	LoadedMapName = FPaths::GetBaseFilename(RelativePath);
	BSP->LogSummary(LoadedMapName);

	MaterialLibrary = NewObject<ULambdaMaterialLibrary>(this);
	MaterialLibrary->Initialize();

	// CPropData::LevelInitPreEntity: how much punishment each kind of prop takes, read before any prop spawns.
	FSourcePropData::Get().Load();

	Stats = FSourceBSPLoadStats();
	Stats.NumFaces = BSP->Faces.Num();
	Stats.NumEntities = BSP->Entities.Num();

	FLambdaLoadProgress::SetStage(ELambdaLoadStage::BuildingWorld);
	BuildWorldGeometry();
	FLambdaLoadProgress::SetStage(ELambdaLoadStage::SpawningEntities);
	SpawnEntities();

	// Source precaches decals in LevelInitPreEntity and impact sounds with the surface properties; doing it here
	// keeps the first shot from building all of it inside one frame.
	FLambdaLoadProgress::SetStage(ELambdaLoadStage::Precaching);
	SourceImpact::Precache(MaterialLibrary, this);

	Stats.NumMaterials = MaterialLibrary->GetNumMaterials();
	Stats.NumTextures = MaterialLibrary->GetNumTextures();
	Stats.LoadTimeSeconds = (float)(FPlatformTime::Seconds() - StartTime);

	UE_LOG(LogLambdaSource, Log, TEXT("Loaded map '%s' in %.2fs: %d faces (%d rendered, %d collision-only, %d skipped, %d displacement), %d verts, %d tris, %d sections, %d materials, %d textures, %d entities, %d lights, %d player starts, %d brush entities"),
		*LoadedMapName, Stats.LoadTimeSeconds, Stats.NumFaces, Stats.NumRenderedFaces, Stats.NumCollisionOnlyFaces, Stats.NumSkippedFaces, Stats.NumDisplacementFaces,
		Stats.NumVertices, Stats.NumTriangles, Stats.NumSections, Stats.NumMaterials, Stats.NumTextures, Stats.NumEntities, Stats.NumLights, Stats.NumPlayerStarts, Stats.NumBrushEntities);
	LogUnhandledEntities();
	FLambdaLoadProgress::End();
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
	Entities.Reset();
	EventQueue.Reset();
	BSP.Reset();
	LoadedMapName.Reset();
	MaterialLibrary = nullptr;
	// The models built for the last map are cached across the whole process, but the materials on them belong to
	// the library above, which has just been let go. Keeping the meshes would keep meshes whose materials are
	// about to be collected, and a model whose materials have gone is drawn in the default grey.
	FSourceSkeletalMesh::FlushCache();
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

	// MapEntity_ParseAllEntities does the templates before anything else: a point_template takes the keyvalues of
	// the entities it names, and those entities are then removed, so they are never in the map to begin with.
	TArray<ASourcePointTemplate*> PointTemplates;
	for (const FSourceEntity& Entity : BSP->Entities)
	{
		if (!Entity.ClassName.Equals(TEXT("point_template"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.ObjectFlags |= RF_Transient;
		if (ASourcePointTemplate* Template = World->SpawnActor<ASourcePointTemplate>(
			ASourcePointTemplate::StaticClass(), FTransform::Identity, Params))
		{
			Template->InitializeEntity(Entity, this);
			RegisterEntity(Template);
			SpawnedActors.Add(Template);
			PointTemplates.Add(Template);
		}
	}

	// StartBuildingTemplates + AddTemplate: hand each template the entities it names, and take them out of the map.
	TSet<int32> TemplatedEntities;
	for (ASourcePointTemplate* Template : PointTemplates)
	{
		int32 Found = 0;
		for (int32 Index = 0; Index < BSP->Entities.Num(); ++Index)
		{
			const FSourceEntity& Candidate = BSP->Entities[Index];
			if (Candidate.ClassName.Equals(TEXT("point_template"), ESearchCase::IgnoreCase))
			{
				continue;
			}
			const FString Name = Candidate.Get(TEXT("targetname"));
			if (Name.IsEmpty() || !Template->OwnsEntityNamed(Name))
			{
				continue;
			}
			Template->AddTemplate(Candidate);
			++Found;
			if (Template->ShouldRemoveTemplateEntities())
			{
				TemplatedEntities.Add(Index);
			}
		}
		if (Found == 0)
		{
			UE_LOG(LogLambdaSource, Warning,
				TEXT("point_template '%s' names entities that are not in the map"), *Template->GetTargetName());
		}
		else
		{
			UE_LOG(LogLambdaSource, Log, TEXT("point_template '%s' holds %d entities%s"),
				*Template->GetTargetName(), Found,
				Template->ShouldRemoveTemplateEntities() ? TEXT(" (removed from the map)") : TEXT(""));
		}
	}

	const int32 NumEntities = FMath::Max(1, BSP->Entities.Num());
	for (int32 EntityIndex = 0; EntityIndex < BSP->Entities.Num(); ++EntityIndex)
	{
		// This is where the models are loaded and the materials compiled, so it is where the bar spends most of
		// its time. Reporting per entity is what keeps it moving instead of sitting still and then jumping.
		FLambdaLoadProgress::SetStageFraction((float)EntityIndex / (float)NumEntities);
		if (TemplatedEntities.Contains(EntityIndex))
		{
			continue;
		}
		const FSourceEntity& Entity = BSP->Entities[EntityIndex];
		if (Entity.ClassName.Equals(TEXT("point_template"), ESearchCase::IgnoreCase))
		{
			continue;	// built above
		}
		SpawnEntityFromKeyValues(Entity);
	}

	const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();
	if (!bSpawnedSkyLight && Settings.AmbientFillIntensity > 0.0f)
	{
		SpawnAmbientFill(Settings.AmbientFillColor, Settings.AmbientFillIntensity);
	}
}

/**
 * MapEntity_ParseEntity: turn one entity's keyvalues into a live actor. The map load runs every entity through
 * here, and so does point_template when it stamps out a copy - which is the point of it being separate: a
 * templated entity is spawned by exactly the same path as one written into the map.
 */
AActor* ASourceBSPWorldActor::SpawnEntityFromKeyValues(const FSourceEntity& Entity)
{
	UWorld* World = GetWorld();
	if (!World || !BSP.IsValid())
	{
		return nullptr;
	}
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
			return nullptr;
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
		else if (TSubclassOf<ASourceNPCBase> NPCClass = NPCClassForName(Class))
		{
			return SpawnNPC(Entity, NPCClass);
		}
		else if (ASourceItem::IsItemClass(Class))
		{
			SpawnItem(Entity);
		}
		else if (ASourcePropPhysics::IsPropClass(Class))
		{
			SpawnPropPhysics(Entity);
		}
		else
		{
			UnhandledEntityCounts.FindOrAdd(Class)++;
		}
	}
	return nullptr;
}

TSubclassOf<ASourceNPCBase> ASourceBSPWorldActor::NPCClassForName(const FString& ClassName)
{
	if (ClassName.Equals(TEXT("npc_headcrab"), ESearchCase::IgnoreCase))
	{
		return ASourceNPCHeadcrab::StaticClass();
	}
	if (ClassName.Equals(TEXT("npc_zombie"), ESearchCase::IgnoreCase))
	{
		return ASourceNPCZombie::StaticClass();
	}
	if (ClassName.Equals(TEXT("npc_barnacle"), ESearchCase::IgnoreCase))
	{
		return ASourceNPCBarnacle::StaticClass();
	}
	return nullptr;
}

AActor* ASourceBSPWorldActor::CreateNPC(const FString& ClassName, const FVector& FeetLocation, float YawDegrees)
{
	TSubclassOf<ASourceNPCBase> NPCClass = NPCClassForName(ClassName);
	if (!NPCClass)
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("npc_create: no NPC named '%s'"), *ClassName);
		return nullptr;
	}
	// Hand it the same keyvalues the entity lump would: origin and angles in Source's units and frame.
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const FVector3f Origin = FSourceCoords::ToSource(FeetLocation, Scale);
	FSourceEntity Entity;
	Entity.ClassName = ClassName;
	Entity.Pairs.Emplace(TEXT("classname"), ClassName);
	Entity.Pairs.Emplace(TEXT("origin"), FString::Printf(TEXT("%g %g %g"), Origin.X, Origin.Y, Origin.Z));
	// CreatePhysicsProp spawns physics props unrotated and lets them settle.
	Entity.Pairs.Emplace(TEXT("angles"), FString::Printf(TEXT("0 %g 0"), YawDegrees));	// UE yaw -> Source yaw
	return SpawnNPC(Entity, NPCClass);
}

AActor* ASourceBSPWorldActor::CreateProp(const FString& ModelPath, const FVector& Location, float YawDegrees)
{
	// prop_physics_create builds the entity the map would have held, then spawns it.
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	FSourceEntity Entity;
	Entity.ClassName = TEXT("prop_physics");
	// CC_Prop_Physics_Create takes a path under models/ and defaults the extension: "props_junk/wood_crate001a".
	FString Model = ModelPath;
	if (!Model.StartsWith(TEXT("models/"), ESearchCase::IgnoreCase))
	{
		Model = TEXT("models/") + Model;
	}
	if (!Model.EndsWith(TEXT(".mdl"), ESearchCase::IgnoreCase))
	{
		Model += TEXT(".mdl");
	}
	Entity.Pairs.Emplace(TEXT("model"), Model);
	const FVector3f Source = FSourceCoords::ToSource(Location, Scale);
	Entity.Pairs.Emplace(TEXT("origin"), FString::Printf(TEXT("%g %g %g"), Source.X, Source.Y, Source.Z));
	Entity.Pairs.Emplace(TEXT("angles"), FString::Printf(TEXT("0 %g 0"), -YawDegrees));
	return SpawnPropPhysics(Entity);
}

ASourcePropPhysics* ASourceBSPWorldActor::SpawnPropPhysics(const FSourceEntity& Entity)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ASourcePropPhysics* Prop = World->SpawnActor<ASourcePropPhysics>(ASourcePropPhysics::StaticClass(), FTransform::Identity, Params);
	if (!Prop)
	{
		return nullptr;
	}
	Prop->InitializeFromEntity(Entity, MaterialLibrary);
	if (IsValid(Prop))
	{
		++Stats.NumProps;
	}
	return Prop;
}

ASourceItem* ASourceBSPWorldActor::SpawnItem(const FSourceEntity& Entity)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ASourceItem* Item = World->SpawnActor<ASourceItem>(ASourceItem::StaticClass(), FTransform::Identity, Params);
	if (!Item)
	{
		return nullptr;
	}
	// The item drops itself to the floor and sizes its own touch box from the model it loads.
	Item->InitializeFromEntity(Entity, MaterialLibrary);
	if (IsValid(Item))
	{
		++Stats.NumItems;
	}
	return Item;
}

ASourceNPCBase* ASourceBSPWorldActor::SpawnNPC(const FSourceEntity& Entity, TSubclassOf<ASourceNPCBase> NPCClass)
{
	UWorld* World = GetWorld();
	if (!World || !NPCClass)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;
	ASourceNPCBase* NPC = World->SpawnActor<ASourceNPCBase>(NPCClass, FTransform::Identity, Params);
	if (!NPC)
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Could not spawn %s"), *Entity.ClassName);
		return nullptr;
	}
	NPC->InitializeFromEntity(Entity, this, MaterialLibrary);
	SpawnedActors.Add(NPC);
	++Stats.NumNPCs;
	UE_LOG(LogLambdaSource, Log, TEXT("%s at %s"), *Entity.ClassName, *NPC->GetActorLocation().ToString());
	return NPC;
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
	else if (Class.Equals(TEXT("func_button"), ESearchCase::IgnoreCase))
	{
		ActorClass = ASourceFuncButton::StaticClass();
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
	BrushActor->InitializeFromEntity(*BSP, ModelIndex, Entity, MaterialLibrary, this);
	RegisterEntity(BrushActor);
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

// ---------------------------------------------------------------------------------------------------------------------
// Entity I/O
// ---------------------------------------------------------------------------------------------------------------------

void ASourceBSPWorldActor::RegisterEntity(ASourceEntity* InEntity)
{
	if (InEntity)
	{
		Entities.Add(InEntity);
	}
}

void ASourceBSPWorldActor::QueueEntityEvent(const FString& Target, const FString& Input, const FString& Parameter,
	AActor* Activator, AActor* Caller, float Delay)
{
	if (Target.IsEmpty() || Input.IsEmpty())
	{
		return;
	}
	FSourceQueuedEvent Event;
	Event.Target = Target;
	Event.Input = Input;
	Event.Parameter = Parameter;
	Event.Activator = Activator;
	Event.Caller = Caller;
	Event.FireTime = GetWorld()->GetTimeSeconds() + FMath::Max(0.0f, Delay);
	EventQueue.Add(MoveTemp(Event));

	UE_LOG(LogLambdaSource, Verbose, TEXT("I/O queued: %s.%s(%s) in %gs"), *Target, *Input, *Parameter, Delay);
}

void ASourceBSPWorldActor::ResolveTargets(const FString& Target, AActor* Activator, AActor* Caller,
	TArray<ASourceEntity*>& Out) const
{
	// Source's magic target names.
	if (Target.Equals(TEXT("!activator"), ESearchCase::IgnoreCase))
	{
		if (ASourceEntity* AsEntity = Cast<ASourceEntity>(Activator)) { Out.Add(AsEntity); }
		return;
	}
	if (Target.Equals(TEXT("!caller"), ESearchCase::IgnoreCase) || Target.Equals(TEXT("!self"), ESearchCase::IgnoreCase))
	{
		if (ASourceEntity* AsEntity = Cast<ASourceEntity>(Caller)) { Out.Add(AsEntity); }
		return;
	}

	for (const TObjectPtr<ASourceEntity>& Candidate : Entities)
	{
		if (IsValid(Candidate) && Candidate->MatchesTargetName(Target))
		{
			Out.Add(Candidate);
		}
	}
}

void ASourceBSPWorldActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (EventQueue.Num() == 0)
	{
		return;
	}

	// CEventQueue::ServiceEvents - deliver everything whose time has come. Delivering an input can queue more
	// events, so collect the due ones first and let those land on a later tick.
	const float Now = GetWorld()->GetTimeSeconds();
	TArray<FSourceQueuedEvent> Due;
	for (int32 i = EventQueue.Num() - 1; i >= 0; --i)
	{
		if (EventQueue[i].FireTime <= Now)
		{
			Due.Add(EventQueue[i]);
			EventQueue.RemoveAtSwap(i);
		}
	}

	for (const FSourceQueuedEvent& Event : Due)
	{
		TArray<ASourceEntity*> Targets;
		ResolveTargets(Event.Target, Event.Activator.Get(), Event.Caller.Get(), Targets);
		if (Targets.Num() == 0)
		{
			UE_LOG(LogLambdaSource, Warning, TEXT("I/O: no entity named '%s' for input '%s'"), *Event.Target, *Event.Input);
			continue;
		}
		for (ASourceEntity* Target : Targets)
		{
			UE_LOG(LogLambdaSource, Verbose, TEXT("I/O fire: %s.%s(%s)"), *Event.Target, *Event.Input, *Event.Parameter);
			Target->AcceptInput(Event.Input, Event.Activator.Get(), Event.Caller.Get(), Event.Parameter);
		}
	}
}
