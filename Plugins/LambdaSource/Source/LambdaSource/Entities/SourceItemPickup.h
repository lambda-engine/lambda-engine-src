#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "SourceItemPickup.generated.h"

UINTERFACE(MinimalAPI)
class USourceItemPickup : public UInterface
{
	GENERATED_BODY()
};

/**
 * What a CItem needs of the player to hand itself over: CBasePlayer::GiveAmmo and CBasePlayer::BumpWeapon.
 * Implemented by the player character in the game module.
 */
class LAMBDASOURCE_API ISourceItemPickup
{
	GENERATED_BODY()

public:
	/** CBasePlayer::GiveAmmo: returns how much was actually taken (0 when already carrying the maximum). */
	virtual int32 GiveAmmo(const FString& AmmoType, int32 Count) = 0;
	/**
	 * CBasePlayer::BumpWeapon: takes the weapon if it is not carried yet, otherwise takes its ammo. False leaves
	 * the weapon on the floor.
	 */
	virtual bool BumpWeapon(const FString& WeaponClassName) = 0;
};
