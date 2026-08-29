#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralMeshComponent.h"
#include "Formats/SourceBSPFile.h"
#include "World/SourceEntityIO.h"
#include "World/SourceGeometryBuilder.h"
#include "SourceBSPWorldActor.generated.h"

class ULambdaMaterialLibrary;
class ASourceEntity;
class ASourceNPCBase;
class ULightComponent;
class ULocalLightComponent;

/** Statistics of the last LoadMap call (for logging / on-screen debug). */
USTRUCT(BlueprintType)
struct FSourceBSPLoadStats
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Lambda") int32 NumFaces = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Lambda") int32 NumRenderedFaces = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Lambda") int32 NumCollisionOnlyFaces = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Lambda") int32 NumSkippedFaces = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Lambda") int32 NumDisplacementFaces = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Lambda") int32 NumVertices = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Lambda") int32 NumTriangles = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Lambda") int32 NumSections = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Lambda") int32 NumMaterials = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Lambda") int32 NumTextures = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Lambda") int32 NumEntities = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Lambda") int32 NumLights = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Lambda") int32 NumPlayerStarts = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Lambda") int32 NumNPCs = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Lambda") int32 NumItems = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Lambda") int32 NumProps = 0;
	int32 NumBrushEntities = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Lambda") float LoadTimeSeconds = 0.0f;
};

/**
 * Builds a playable UE world from a compiled Source BSP at runtime: world geometry (model 0) as procedural mesh sections
 * with collision, materials from VMT/VTF, player starts and lights from the entity lump.
 */
UCLASS()
class LAMBDASOURCE_API ASourceBSPWorldActor : public AActor
{
	GENERATED_BODY()

public:
	ASourceBSPWorldActor();

	/** Loads maps/<MapName>.bsp from the game directories and builds the world. */
	UFUNCTION(BlueprintCallable, Category = "Lambda")
	bool LoadMap(const FString& MapName);

	/** Loads a BSP by relative content path (e.g. "maps/test.bsp"). */
	bool LoadBSPFile(const FString& RelativePath);

	/** Removes all generated geometry and spawned entity actors. */
	UFUNCTION(BlueprintCallable, Category = "Lambda")
	void ClearMap();

	UFUNCTION(BlueprintPure, Category = "Lambda")
	bool IsMapLoaded() const { return BSP.IsValid() && BSP->IsLoaded(); }

	UFUNCTION(BlueprintPure, Category = "Lambda")
	FString GetLoadedMapName() const { return LoadedMapName; }

	UFUNCTION(BlueprintPure, Category = "Lambda")
	const FSourceBSPLoadStats& GetLoadStats() const { return Stats; }

	const FSourceBSPFile* GetBSP() const { return BSP.Get(); }

	// ---- Entity I/O (CEventQueue) ----
	/** Queues an input to be delivered to every entity matching Target after Delay seconds. */
	void QueueEntityEvent(const FString& Target, const FString& Input, const FString& Parameter,
		AActor* Activator, AActor* Caller, float Delay);

	/** CEventQueue::CancelEvents - forgets every queued event this entity fired. */
	void CancelQueuedEventsFrom(const AActor* Caller);
	/** Resolves a target name, including Source's !activator / !caller / !self / !player keywords. */
	void ResolveTargets(const FString& Target, AActor* Activator, AActor* Caller, TArray<ASourceEntity*>& Out) const;
	/** Registers an entity so it can be found by targetname. */
	void RegisterEntity(ASourceEntity* InEntity);

	/**
	 * npc_create: spawns an NPC by classname at a world location (its feet) facing Yaw, as if the map had placed
	 * it there. Returns null for classnames without an NPC implementation.
	 */
	AActor* CreateNPC(const FString& ClassName, const FVector& FeetLocation, float YawDegrees);

	/** Spawns one entity from its keyvalues; point_template uses this to stamp out its copies. */
	AActor* SpawnEntityFromKeyValues(const FSourceEntity& Entity);
	/** prop_physics_create: drops a physics prop of that model at a spot. */
	AActor* CreateProp(const FString& ModelPath, const FVector& Location, float YawDegrees);
	TArray<const FSourceEntity*> FindEntities(const FString& ClassName) const;

	virtual void Tick(float DeltaSeconds) override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lambda")
	TObjectPtr<USourceBrushMeshComponent> WorldMesh;

	UPROPERTY(Transient)
	TObjectPtr<ULambdaMaterialLibrary> MaterialLibrary;

	/** Actors spawned from entities (lights, player starts...). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<AActor>> SpawnedActors;

private:
	void BuildWorldGeometry();
	/** Spawns a point NPC entity (npc_*) of the given class from its keyvalues. */
	ASourceNPCBase* SpawnNPC(const FSourceEntity& Entity, TSubclassOf<ASourceNPCBase> NPCClass);
	/** CItem: an ammo box or a weapon lying in the map (game/server/item_world.cpp). */
	class ASourceItem* SpawnItem(const FSourceEntity& Entity);
	/** prop_physics: a model the physics owns (game/server/props.cpp). */
	class ASourcePropPhysics* SpawnPropPhysics(const FSourceEntity& Entity);
	/** The NPC class registered for a classname, or null. */
	static TSubclassOf<ASourceNPCBase> NPCClassForName(const FString& ClassName);
	/** Spawns the actor for a brush entity ("model" "*N"), choosing the class from its classname. */
	void SpawnBrushEntity(const FSourceEntity& Entity, int32 ModelIndex);
	void SpawnEntities();
	/**
	 * Puts a navmesh over the map that has just been built.
	 *
	 * Source's NPCs walk a node graph the mapper placed by hand, compiled into an .ain beside the BSP. Nothing
	 * here is compiled ahead of time and most maps have no nodes in them, so navigation is generated from the
	 * geometry instead, once, after it exists.
	 */
	void BuildNavigation();

	/** Registers the player pawn as a navigation invoker, once there is one. */
	void RegisterPlayerAsNavInvoker();
	bool bNavigationReady = false;
	bool bPlayerRegisteredAsInvoker = false;

	/** How far around an invoker navigation is generated, and the distance at which it is dropped again. */
	static constexpr float NavInvokerGenerationRadius = 6000.0f;	// cm
	static constexpr float NavInvokerRemovalRadius = 9000.0f;
	void SpawnPlayerStart(const FSourceEntity& Entity);
	void SpawnPointLight(const FSourceEntity& Entity);
	/** A point entity the game module implements that needs nothing built for it - logic_relay and its kind. */
	AActor* SpawnGamePointEntity(const FSourceEntity& Entity);
	void SpawnSpotLight(const FSourceEntity& Entity);
	void SpawnEnvironmentLight(const FSourceEntity& Entity);
	void SpawnAmbientFill(const FLinearColor& Color, float Intensity);
	void ConfigureLocalLight(ULocalLightComponent* Light, const FSourceEntity& Entity) const;
	static void GetLightDirectionAngles(const FSourceEntity& Entity, float& OutPitch, float& OutYaw);
	static bool ParseLightColor(const FSourceEntity& Entity, const TCHAR* Key, FLinearColor& OutColor, float& OutBrightness);
	void LogUnhandledEntities() const;

	TSharedPtr<FSourceBSPFile> BSP;
	FString LoadedMapName;
	FSourceBSPLoadStats Stats;
	TMap<FString, int32> UnhandledEntityCounts;

	/** Every entity spawned from the map, for targetname lookups. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<ASourceEntity>> Entities;

	/** Pending I/O events, serviced each tick (CEventQueue::ServiceEvents). */
	TArray<FSourceQueuedEvent> EventQueue;
	bool bSpawnedSkyLight = false;
};
