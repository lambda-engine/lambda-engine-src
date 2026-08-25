#pragma once

#include "CoreMinimal.h"

/**
 * One weapon's data from scripts/weapon_<name>.txt, a port of the fields FileWeaponInfo_t reads in
 * game/shared/weapon_parse.cpp. Both the game and client read this in Source; here it drives the runtime weapons.
 */
struct LAMBDASOURCE_API FSourceWeaponInfo
{
	FString ClassName;			// "weapon_pistol" (from the script's filename)
	FString PrintName;			// "#HL2_Pistol"
	FString ViewModel;			// models/weapons/v_pistol.mdl
	FString PlayerModel;		// models/weapons/w_pistol.mdl
	FString AnimPrefix;

	int32 Bucket = 0;
	int32 BucketPosition = 0;
	int32 ClipSize = 0;			// -1 / WEAPON_NOCLIP when the weapon does not use clips
	int32 Clip2Size = 0;
	FString PrimaryAmmo = TEXT("None");
	FString SecondaryAmmo = TEXT("None");
	int32 Weight = 0;
	int32 ItemFlags = 0;

	/** SoundData block: "single_shot" -> "Weapon_Pistol.Single" (a soundscript name). */
	TMap<FString, FString> Sounds;

	bool UsesClipsForAmmo1() const { return ClipSize > 0; }
	/** Returns the soundscript name for a SoundData key, or empty. */
	FString GetSound(const FString& Key) const;
};

/**
 * Loads every weapon script listed in scripts/weapon_manifest.txt, so weapons are defined by data exactly as they
 * are in Source rather than hardcoded.
 */
class LAMBDASOURCE_API FSourceWeaponScripts
{
public:
	static FSourceWeaponScripts& Get();

	void Initialize();
	void Reset() { Weapons.Reset(); bInitialized = false; }

	/** Looks up by weapon classname ("weapon_pistol"). */
	const FSourceWeaponInfo* Find(const FString& ClassName);

	int32 Num() const { return Weapons.Num(); }

private:
	FSourceWeaponScripts() = default;
	void LoadWeaponFile(const FString& RelativePath);

	TMap<FString, FSourceWeaponInfo> Weapons;
	bool bInitialized = false;
};
