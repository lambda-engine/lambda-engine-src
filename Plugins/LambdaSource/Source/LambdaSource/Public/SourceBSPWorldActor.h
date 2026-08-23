#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralMeshComponent.h"
#include "SourceBSPFile.h"
#include "SourceBSPWorldActor.generated.h"

class ULambdaMaterialLibrary;
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
	UPROPERTY(BlueprintReadOnly, Category = "Lambda") int32 NumBrushEntities = 0;
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
	TArray<const FSourceEntity*> FindEntities(const FString& ClassName) const;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lambda")
	TObjectPtr<UProceduralMeshComponent> WorldMesh;

	UPROPERTY(Transient)
	TObjectPtr<ULambdaMaterialLibrary> MaterialLibrary;

	/** Actors spawned from entities (lights, player starts...). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<AActor>> SpawnedActors;

private:
	void BuildWorldGeometry();
	/** Spawns the actor for a brush entity ("model" "*N"), choosing the class from its classname. */
	void SpawnBrushEntity(const FSourceEntity& Entity, int32 ModelIndex);
	void SpawnEntities();
	void SpawnPlayerStart(const FSourceEntity& Entity);
	void SpawnPointLight(const FSourceEntity& Entity);
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
	bool bSpawnedSkyLight = false;
};
