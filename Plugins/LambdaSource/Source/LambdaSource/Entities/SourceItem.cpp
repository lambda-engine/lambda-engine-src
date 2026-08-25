#include "Entities/SourceItem.h"
#include "Materials/LambdaMaterialLibrary.h"
#include "Audio/LambdaSoundLibrary.h"
#include "Core/LambdaSourceModule.h"
#include "Core/LambdaSourceSettings.h"
#include "Core/SourceCoordinates.h"
#include "Entities/SourceItemPickup.h"
#include "Audio/SourceSoundScript.h"
#include "Rendering/SourceStudioModelComponent.h"
#include "Weapons/SourceWeaponScript.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundAttenuation.h"

namespace
{
	/**
	 * game/server/hl2/item_ammo.cpp: every ammo box, the model it uses and what it hands over. The counts are
	 * items.h's SIZE_AMMO_*; the ammo names index the ammo table (FSourceAmmoDef), which is what the weapon
	 * scripts also name.
	 */
	const FSourceAmmoItem GAmmoItems[] =
	{
		{ TEXT("item_ammo_pistol"),			TEXT("models/items/boxsrounds.mdl"),					TEXT("Pistol"),			20 },
		{ TEXT("item_box_srounds"),			TEXT("models/items/boxsrounds.mdl"),					TEXT("Pistol"),			20 },
		{ TEXT("item_ammo_pistol_large"),	TEXT("models/items/boxsrounds.mdl"),					TEXT("Pistol"),			100 },
		{ TEXT("item_large_box_srounds"),	TEXT("models/items/boxsrounds.mdl"),					TEXT("Pistol"),			100 },
		{ TEXT("item_ammo_smg1"),			TEXT("models/items/boxmrounds.mdl"),					TEXT("SMG1"),			45 },
		{ TEXT("item_box_mrounds"),			TEXT("models/items/boxmrounds.mdl"),					TEXT("SMG1"),			45 },
		{ TEXT("item_ammo_smg1_large"),		TEXT("models/items/boxmrounds.mdl"),					TEXT("SMG1"),			225 },
		{ TEXT("item_large_box_mrounds"),	TEXT("models/items/boxmrounds.mdl"),					TEXT("SMG1"),			225 },
		{ TEXT("item_ammo_ar2"),			TEXT("models/items/combine_rifle_cartridge01.mdl"),		TEXT("AR2"),			20 },
		{ TEXT("item_box_lrounds"),			TEXT("models/items/combine_rifle_cartridge01.mdl"),		TEXT("AR2"),			20 },
		{ TEXT("item_ammo_ar2_large"),		TEXT("models/items/combine_rifle_cartridge01.mdl"),		TEXT("AR2"),			100 },
		{ TEXT("item_large_box_lrounds"),	TEXT("models/items/combine_rifle_cartridge01.mdl"),		TEXT("AR2"),			100 },
		{ TEXT("item_ammo_357"),			TEXT("models/items/357ammo.mdl"),						TEXT("357"),			6 },
		{ TEXT("item_ammo_357_large"),		TEXT("models/items/357ammobox.mdl"),					TEXT("357"),			20 },
		{ TEXT("item_ammo_crossbow"),		TEXT("models/items/crossbowrounds.mdl"),				TEXT("XBowBolt"),		6 },
		{ TEXT("item_box_buckshot"),		TEXT("models/items/boxbuckshot.mdl"),					TEXT("Buckshot"),		20 },
		{ TEXT("item_ammo_smg1_grenade"),	TEXT("models/items/ar2_grenade.mdl"),					TEXT("SMG1_Grenade"),	1 },
		{ TEXT("item_ar2_grenade"),			TEXT("models/items/ar2_grenade.mdl"),					TEXT("SMG1_Grenade"),	1 },
		{ TEXT("item_rpg_round"),			TEXT("models/weapons/w_missile_closed.mdl"),			TEXT("RPG_Round"),		1 },
	};

	/** ITEM_PICKUP_BOX_BLOAT: how far outside its own bounds an item can be picked up from. */
	constexpr float ITEM_PICKUP_BOX_BLOAT = 24.0f;
}

const FSourceAmmoItem* ASourceItem::FindAmmoItem(const FString& ClassName)
{
	for (const FSourceAmmoItem& Item : GAmmoItems)
	{
		if (ClassName.Equals(Item.ClassName, ESearchCase::IgnoreCase))
		{
			return &Item;
		}
	}
	return nullptr;
}

bool ASourceItem::IsItemClass(const FString& ClassName)
{
	// Weapons lying in a map are entities named after the weapon they give; the weapon scripts decide whether one
	// exists, so anything "weapon_" is offered here and dropped again if the script is missing.
	return FindAmmoItem(ClassName) != nullptr || ClassName.StartsWith(TEXT("weapon_"), ESearchCase::IgnoreCase);
}

ASourceItem::ASourceItem(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = false;

	PickupBox = CreateDefaultSubobject<UBoxComponent>(TEXT("PickupBox"));
	SetRootComponent(PickupBox);
	PickupBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	PickupBox->SetCollisionObjectType(ECC_WorldDynamic);
	PickupBox->SetCollisionResponseToAllChannels(ECR_Ignore);
	PickupBox->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	PickupBox->SetGenerateOverlapEvents(true);

	Model = CreateDefaultSubobject<USourceStudioModelComponent>(TEXT("WorldModel"));
	Model->SetupAttachment(PickupBox);
	Model->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Model->SetCastShadow(true);
	Model->SetMobility(EComponentMobility::Movable);
}

void ASourceItem::InitializeFromEntity(const FSourceEntity& InEntity, ULambdaMaterialLibrary* Materials)
{
	Entity = InEntity;
	MaterialLibrary = Materials;
	const float Scale = ULambdaSourceSettings::Get().UnitScale;

	FString ModelPath;
	AmmoItem = FindAmmoItem(Entity.ClassName);
	if (AmmoItem)
	{
		ModelPath = AmmoItem->Model;
	}
	else if (Entity.ClassName.StartsWith(TEXT("weapon_"), ESearchCase::IgnoreCase))
	{
		// CBaseCombatWeapon lying in the world draws its "playermodel" (w_*.mdl), from the weapon's script.
		WeaponClassName = Entity.ClassName;
		const FSourceWeaponInfo* Info = FSourceWeaponScripts::Get().Find(WeaponClassName);
		if (Info)
		{
			ModelPath = Info->PlayerModel;
		}
	}

	if (ModelPath.IsEmpty() || !Model->SetModel(ModelPath, MaterialLibrary))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("%s: no world model ('%s'); not spawning the item"), *Entity.ClassName, *ModelPath);
		Destroy();
		return;
	}
	Model->PlayActivity(TEXT("ACT_IDLE"));

	// CItem::Spawn's touch bounds: the model's own box, bloated so walking near is enough.
	const FVector3f HullMin = Model->GetModel()->GetHullMin();
	const FVector3f HullMax = Model->GetModel()->GetHullMax();
	const FVector Extent = FVector(
		FMath::Max(FMath::Abs(HullMin.X), FMath::Abs(HullMax.X)),
		FMath::Max(FMath::Abs(HullMin.Y), FMath::Abs(HullMax.Y)),
		FMath::Max(FMath::Abs(HullMin.Z), FMath::Abs(HullMax.Z))) * Scale
		+ FVector(ITEM_PICKUP_BOX_BLOAT * Scale);
	PickupBox->SetBoxExtent(FVector(FMath::Max(Extent.X, 8.0f), FMath::Max(Extent.Y, 8.0f), FMath::Max(Extent.Z, 8.0f)));

	FVector3f Origin = FVector3f::ZeroVector;
	Entity.GetVector(TEXT("origin"), Origin);
	FVector3f Angles = FVector3f::ZeroVector;
	Entity.GetVector(TEXT("angles"), Angles);
	const FVector Spot = FSourceCoords::ToUE(Origin, Scale);
	SetActorLocation(Spot);
	SetActorRotation(FSourceCoords::AnglesToUE(Angles));
	// The model hangs off the box's centre, so its own origin sits where the entity is.
	Model->SetRelativeLocation(FVector::ZeroVector);

	// MOVETYPE_FLYGRAVITY: mappers drop items roughly and Source lets them fall the last few units to the floor.
	if (UWorld* World = GetWorld())
	{
		FHitResult Hit;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(ItemDrop), false, this);
		const float DropUnits = 128.0f * Scale;
		if (World->LineTraceSingleByChannel(Hit, Spot + FVector(0, 0, 2.0f * Scale), Spot - FVector(0, 0, DropUnits), ECC_Visibility, Params))
		{
			SetActorLocation(Hit.ImpactPoint);
		}
	}

	UE_LOG(LogLambdaSource, Log, TEXT("%s at %s: '%s'%s"), *Entity.ClassName, *GetActorLocation().ToString(), *ModelPath,
		AmmoItem ? *FString::Printf(TEXT(" (%d %s)"), AmmoItem->Count, AmmoItem->AmmoType) : TEXT(""));
}

void ASourceItem::BeginPlay()
{
	Super::BeginPlay();
	PickupBox->OnComponentBeginOverlap.AddDynamic(this, &ASourceItem::OnPickupOverlap);

	// A player already standing in it when the map loads should still get it.
	TArray<AActor*> Overlapping;
	PickupBox->GetOverlappingActors(Overlapping, APawn::StaticClass());
	for (AActor* Other : Overlapping)
	{
		if (APawn* Pawn = Cast<APawn>(Other))
		{
			if (Pawn->IsPlayerControlled() && MyTouch(Pawn))
			{
				return;
			}
		}
	}
}

void ASourceItem::OnPickupOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
	int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	// CItem::ItemTouch: players only.
	APawn* Pawn = Cast<APawn>(OtherActor);
	if (Pawn && Pawn->IsPlayerControlled())
	{
		MyTouch(Pawn);
	}
}

bool ASourceItem::MyTouch(APawn* Player)
{
	ISourceItemPickup* Pickup = Cast<ISourceItemPickup>(Player);
	if (!Pickup)
	{
		return false;
	}

	bool bTaken = false;
	FString PickupSound;
	if (AmmoItem)
	{
		// ITEM_GiveAmmo -> CBasePlayer::GiveAmmo: nothing happens when the player is already carrying the maximum,
		// and the box stays on the floor for later.
		const int32 Given = Pickup->GiveAmmo(AmmoItem->AmmoType, AmmoItem->Count);
		bTaken = Given > 0;
		PickupSound = TEXT("BaseCombatCharacter.AmmoPickup");
	}
	else if (!WeaponClassName.IsEmpty())
	{
		// CBasePlayer::BumpWeapon: a weapon already carried only hands over its ammo.
		bTaken = Pickup->BumpWeapon(WeaponClassName);
		PickupSound = TEXT("BaseCombatWeapon.WeaponPickup");
	}

	if (!bTaken)
	{
		return false;
	}

	if (!PickupSound.IsEmpty())
	{
		float Volume = 1.0f, Pitch = 1.0f;
		if (ULambdaSoundWave* Wave = FLambdaSoundCache::Get().CreateWaveResolved(this, PickupSound, false, Volume, Pitch))
		{
			const FSourceSoundScriptEntry* Entry = FSourceSoundScripts::Get().Find(PickupSound);
			UGameplayStatics::SpawnSoundAtLocation(this, Wave, GetActorLocation(), FRotator::ZeroRotator, Volume, Pitch,
				0.0f, FLambdaSoundCache::Get().GetAttenuationForSoundLevel(Entry ? Entry->SoundLevel : 75.0f));
		}
	}
	UE_LOG(LogLambdaSource, Verbose, TEXT("%s picked up"), *Entity.ClassName);
	Destroy();
	return true;
}
