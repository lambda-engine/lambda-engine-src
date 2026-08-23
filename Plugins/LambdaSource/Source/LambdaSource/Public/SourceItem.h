#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SourceBSPFile.h"
#include "SourceItem.generated.h"

class USourceStudioModelComponent;
class ULambdaMaterialLibrary;
class UBoxComponent;

/** One row of the ammo item table (game/server/hl2/item_ammo.cpp): what the box looks like and what it gives. */
struct FSourceAmmoItem
{
	const TCHAR* ClassName;
	const TCHAR* Model;
	const TCHAR* AmmoType;
	int32 Count;
};

/**
 * CItem (game/server/item_world.cpp): a thing lying on the floor that the player walks into and takes - an ammo
 * box, or a weapon waiting to be picked up. It draws its world model, drops to the ground where the mapper left
 * it, and its touch box is the model's bounds bloated by ITEM_PICKUP_BOX_BLOAT so brushing past is enough.
 *
 * What is deliberately not here: item respawning (a multiplayer rule), the physics push of items you shoot,
 * constrained items, and the +USE pickup path.
 */
UCLASS()
class LAMBDASOURCE_API ASourceItem : public AActor
{
	GENERATED_BODY()

public:
	ASourceItem(const FObjectInitializer& ObjectInitializer);

	/** Reads the entity's keyvalues, loads the model and drops the item to the floor. */
	void InitializeFromEntity(const FSourceEntity& InEntity, ULambdaMaterialLibrary* Materials);

	/** The ammo item table; null when the classname is not one. */
	static const FSourceAmmoItem* FindAmmoItem(const FString& ClassName);
	/** True for classnames this actor can represent (an ammo box or a weapon_ lying in the map). */
	static bool IsItemClass(const FString& ClassName);

protected:
	virtual void BeginPlay() override;

	/** CItem::ItemTouch -> MyTouch: gives what the item holds and removes it, or leaves it if the player is full. */
	UFUNCTION()
	void OnPickupOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
		int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	bool MyTouch(APawn* Player);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lambda")
	TObjectPtr<USourceStudioModelComponent> Model;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lambda")
	TObjectPtr<UBoxComponent> PickupBox;

	UPROPERTY(Transient)
	TObjectPtr<ULambdaMaterialLibrary> MaterialLibrary;

	FSourceEntity Entity;
	/** Set when the item is a weapon lying in the map: the weapon's classname ("weapon_pistol"). */
	FString WeaponClassName;
	const FSourceAmmoItem* AmmoItem = nullptr;
};
