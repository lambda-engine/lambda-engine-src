#include "SourceWeaponScript.h"
#include "LambdaFileSystem.h"
#include "LambdaSourceModule.h"
#include "SourceKeyValues.h"
#include "Misc/Paths.h"

FString FSourceWeaponInfo::GetSound(const FString& Key) const
{
	if (const FString* Found = Sounds.Find(Key.ToLower()))
	{
		return *Found;
	}
	return FString();
}

FSourceWeaponScripts& FSourceWeaponScripts::Get()
{
	static FSourceWeaponScripts Instance;
	return Instance;
}

void FSourceWeaponScripts::Initialize()
{
	if (bInitialized)
	{
		return;
	}
	bInitialized = true;

	TArray<uint8> ManifestBytes;
	if (!FLambdaFileSystem::Get().ReadFile(TEXT("scripts/weapon_manifest.txt"), ManifestBytes))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("No scripts/weapon_manifest.txt found - no weapons will be available"));
		return;
	}

	FSourceKeyValues Manifest;
	FString Error;
	if (!FSourceKeyValues::ParseSingle(ManifestBytes, Manifest, &Error))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("weapon_manifest.txt: %s"), *Error);
		return;
	}

	for (const FSourceKeyValues& Child : Manifest.Children)
	{
		if (Child.Key.Equals(TEXT("file"), ESearchCase::IgnoreCase) && !Child.Value.IsEmpty())
		{
			LoadWeaponFile(Child.Value);
		}
	}

	UE_LOG(LogLambdaSource, Log, TEXT("Weapon scripts: %d weapons"), Weapons.Num());
}

void FSourceWeaponScripts::LoadWeaponFile(const FString& RelativePath)
{
	TArray<uint8> Bytes;
	if (!FLambdaFileSystem::Get().ReadFile(RelativePath, Bytes))
	{
		UE_LOG(LogLambdaSource, Verbose, TEXT("Weapon script not found: %s"), *RelativePath);
		return;
	}

	FSourceKeyValues Root;
	FString Error;
	if (!FSourceKeyValues::ParseSingle(Bytes, Root, &Error))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Weapon script '%s': %s"), *RelativePath, *Error);
		return;
	}

	FSourceWeaponInfo Info;
	// Source derives the weapon's classname from the script filename (weapon_pistol.txt -> weapon_pistol).
	Info.ClassName = FPaths::GetBaseFilename(RelativePath).ToLower();
	Info.PrintName = Root.GetString(TEXT("printname"));
	Info.ViewModel = Root.GetString(TEXT("viewmodel"));
	Info.PlayerModel = Root.GetString(TEXT("playermodel"));
	Info.AnimPrefix = Root.GetString(TEXT("anim_prefix"));
	Info.Bucket = Root.GetInt(TEXT("bucket"), 0);
	Info.BucketPosition = Root.GetInt(TEXT("bucket_position"), 0);
	Info.ClipSize = Root.GetInt(TEXT("clip_size"), 0);
	Info.Clip2Size = Root.GetInt(TEXT("clip2_size"), 0);
	Info.PrimaryAmmo = Root.GetString(TEXT("primary_ammo"), TEXT("None"));
	Info.SecondaryAmmo = Root.GetString(TEXT("secondary_ammo"), TEXT("None"));
	Info.Weight = Root.GetInt(TEXT("weight"), 0);
	Info.ItemFlags = Root.GetInt(TEXT("item_flags"), 0);

	if (const FSourceKeyValues* SoundData = Root.FindChild(TEXT("SoundData")))
	{
		for (const FSourceKeyValues& Sound : SoundData->Children)
		{
			if (!Sound.IsSection() && !Sound.Value.IsEmpty())
			{
				Info.Sounds.Add(Sound.Key.ToLower(), Sound.Value);
			}
		}
	}

	UE_LOG(LogLambdaSource, Verbose, TEXT("  weapon %s: clip=%d ammo=%s sounds=%d"),
		*Info.ClassName, Info.ClipSize, *Info.PrimaryAmmo, Info.Sounds.Num());

	Weapons.Add(Info.ClassName, MoveTemp(Info));
}

const FSourceWeaponInfo* FSourceWeaponScripts::Find(const FString& ClassName)
{
	Initialize();
	return Weapons.Find(ClassName.ToLower());
}
