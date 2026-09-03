#include "LambdaSaveGame.h"

#include "LambdaCharacter.h"
#include "LambdaEngine.h"
#include "LambdaLoadingScreen.h"
#include "LambdaWeapon.h"
#include "Core/SourceCoordinates.h"
#include "Formats/SourceKeyValues.h"
#include "Core/LambdaSourceSettings.h"
#include "FileSystem/LambdaFileSystem.h"
#include "World/SourceBSPWorldActor.h"
#include "GameMapsSettings.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	/**
	 * The state a save carries, parked between the level load and the player existing.
	 *
	 * A restore cannot be applied at load time: OpenLevel tears the world down and the pawn that needs the
	 * state is not built until the new one is up. Source has the same problem and solves it the same way, by
	 * holding the restore data across the transition.
	 */
	struct FPendingRestore
	{
		bool bArmed = false;
		FVector Position = FVector::ZeroVector;
		FRotator Angles = FRotator::ZeroRotator;
		float Health = 100.0f;
		float Armor = 0.0f;
		bool bSuit = true;
		TArray<FString> Weapons;
		FString ActiveWeapon;
		TMap<FString, int32> Ammo;
	};

	FPendingRestore GPending;

	FString CurrentMapName(UWorld* World)
	{
		if (World)
		{
			if (const ASourceBSPWorldActor* BSP =
				Cast<ASourceBSPWorldActor>(UGameplayStatics::GetActorOfClass(World, ASourceBSPWorldActor::StaticClass())))
			{
				return BSP->GetLoadedMapName();
			}
		}
		return FString();
	}
}

FString FLambdaSaveGame::SaveDirectory()
{
	// Source keeps them in the mod's SAVE directory; so do we, so a player finds them where they expect.
	const FString ModDir = FLambdaFileSystem::Get().GetGameDirectory();
	return ModDir.IsEmpty() ? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SAVE"))
		: FPaths::Combine(ModDir, TEXT("SAVE"));
}

FString FLambdaSaveGame::SavePath(const FString& Name)
{
	return FPaths::Combine(SaveDirectory(), Name + TEXT(".sav"));
}

bool FLambdaSaveGame::Save(UWorld* World, const FString& Name)
{
	ALambdaCharacter* Player = Cast<ALambdaCharacter>(UGameplayStatics::GetPlayerPawn(World, 0));
	const FString Map = CurrentMapName(World);
	if (!Player || Map.IsEmpty())
	{
		UE_LOG(LogLambda, Warning, TEXT("save: nothing to save - no player or no map"));
		return false;
	}

	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const FVector3f Pos = FSourceCoords::ToSource(Player->GetActorLocation(), Scale);
	FRotator ViewRot = Player->GetActorRotation();
	if (const APlayerController* PC = Cast<APlayerController>(Player->GetController()))
	{
		ViewRot = PC->GetControlRotation();
	}
	const FVector3f Ang = FSourceCoords::AnglesFromUE(ViewRot);

	// KeyValues, like every other file this engine reads. Readable, diffable, and a modder can hand-edit it.
	TArray<FString> Lines;
	Lines.Add(TEXT("\"lambdasave\""));
	Lines.Add(TEXT("{"));
	Lines.Add(FString::Printf(TEXT("\t\"version\"\t\"1\"")));
	Lines.Add(FString::Printf(TEXT("\t\"map\"\t\"%s\""), *Map));
	Lines.Add(FString::Printf(TEXT("\t\"time\"\t\"%s\""), *FDateTime::Now().ToString()));
	Lines.Add(FString::Printf(TEXT("\t\"origin\"\t\"%.2f %.2f %.2f\""), Pos.X, Pos.Y, Pos.Z));
	Lines.Add(FString::Printf(TEXT("\t\"angles\"\t\"%.2f %.2f %.2f\""), Ang.X, Ang.Y, Ang.Z));
	Lines.Add(FString::Printf(TEXT("\t\"health\"\t\"%.1f\""), Player->GetHealth()));
	Lines.Add(FString::Printf(TEXT("\t\"armor\"\t\"%.1f\""), Player->GetArmor()));
	Lines.Add(FString::Printf(TEXT("\t\"suit\"\t\"%d\""), Player->IsSuitEquipped() ? 1 : 0));

	Lines.Add(TEXT("\t\"weapons\""));
	Lines.Add(TEXT("\t{"));
	int32 Index = 0;
	for (const TObjectPtr<ALambdaWeapon>& Weapon : Player->GetWeapons())
	{
		if (Weapon)
		{
			Lines.Add(FString::Printf(TEXT("\t\t\"%d\"\t\"%s\""), Index++, *Weapon->GetWeaponClassName()));
		}
	}
	Lines.Add(TEXT("\t}"));
	// The ACTIVE weapon, not the selected one: SelectionIndex only means anything while the weapon menu is
	// open, so saving that wrote an empty string and restored a player holding nothing.
	if (const ALambdaWeapon* Active = Player->GetActiveWeapon())
	{
		Lines.Add(FString::Printf(TEXT("\t\"activeweapon\"\t\"%s\""), *Active->GetWeaponClassName()));
	}

	Lines.Add(TEXT("\t\"ammo\""));
	Lines.Add(TEXT("\t{"));
	for (const TPair<FString, int32>& Pair : Player->GetAmmoCounts())
	{
		Lines.Add(FString::Printf(TEXT("\t\t\"%s\"\t\"%d\""), *Pair.Key, Pair.Value));
	}
	Lines.Add(TEXT("\t}"));
	Lines.Add(TEXT("}"));

	const FString Path = SavePath(Name);
	IFileManager::Get().MakeDirectory(*SaveDirectory(), /*Tree=*/ true);
	if (!FFileHelper::SaveStringArrayToFile(Lines, *Path))
	{
		UE_LOG(LogLambda, Warning, TEXT("save: could not write %s"), *Path);
		return false;
	}
	UE_LOG(LogLambda, Log, TEXT("saved '%s' on map '%s'"), *Name, *Map);
	return true;
}

bool FLambdaSaveGame::Load(UWorld* World, const FString& Name)
{
	const FString Path = SavePath(Name);
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogLambda, Warning, TEXT("load: no save called '%s'"), *Name);
		return false;
	}
	FSourceKeyValues Root;
	if (!FSourceKeyValues::ParseSingle(Bytes, Root, nullptr))
	{
		UE_LOG(LogLambda, Warning, TEXT("load: '%s' will not parse"), *Name);
		return false;
	}

	const FString Map = Root.GetString(TEXT("map"));
	if (Map.IsEmpty())
	{
		UE_LOG(LogLambda, Warning, TEXT("load: '%s' names no map"), *Name);
		return false;
	}

	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	FVector3f Origin = FVector3f::ZeroVector;
	FVector3f Angles = FVector3f::ZeroVector;
	FSourceCoords::ParseVector(Root.GetString(TEXT("origin")), Origin);
	FSourceCoords::ParseVector(Root.GetString(TEXT("angles")), Angles);

	GPending = FPendingRestore();
	GPending.bArmed = true;
	GPending.Position = FSourceCoords::ToUE(Origin, Scale);
	GPending.Angles = FSourceCoords::AnglesToUE(Angles);
	GPending.Health = Root.GetFloat(TEXT("health"), 100.0f);
	GPending.Armor = Root.GetFloat(TEXT("armor"), 0.0f);
	GPending.bSuit = Root.GetInt(TEXT("suit"), 1) != 0;
	GPending.ActiveWeapon = Root.GetString(TEXT("activeweapon"));
	if (const FSourceKeyValues* WeaponBlock = Root.FindChild(TEXT("weapons")))
	{
		for (const FSourceKeyValues& Child : WeaponBlock->Children)
		{
			if (!Child.Value.IsEmpty())
			{
				GPending.Weapons.Add(Child.Value);
			}
		}
	}
	if (const FSourceKeyValues* AmmoBlock = Root.FindChild(TEXT("ammo")))
	{
		for (const FSourceKeyValues& Child : AmmoBlock->Children)
		{
			GPending.Ammo.Add(Child.Key, FCString::Atoi(*Child.Value));
		}
	}

	UE_LOG(LogLambda, Log, TEXT("loading '%s': map '%s'"), *Name, *Map);
	if (World)
	{
		FLambdaLoadingScreen::Arm();
		const FString EntryMap = UGameMapsSettings::GetGameDefaultMap();
		UGameplayStatics::OpenLevel(World, FName(*EntryMap), true, FString::Printf(TEXT("map=%s"), *Map));
	}
	return true;
}

void FLambdaSaveGame::ApplyPendingRestore(ALambdaCharacter* Player)
{
	if (!GPending.bArmed || !Player)
	{
		return;
	}
	const FPendingRestore Restore = GPending;
	GPending = FPendingRestore();		// once only; a later respawn is not a restore

	Player->SetActorLocation(Restore.Position, /*bSweep=*/ false, nullptr, ETeleportType::TeleportPhysics);
	if (APlayerController* PC = Cast<APlayerController>(Player->GetController()))
	{
		PC->SetControlRotation(Restore.Angles);
	}
	Player->RestoreSavedState(Restore.Health, Restore.Armor, Restore.bSuit,
		Restore.Weapons, Restore.ActiveWeapon, Restore.Ammo);
	UE_LOG(LogLambda, Log, TEXT("restored player: %.0f health, %.0f armour, %d weapons"),
		Restore.Health, Restore.Armor, Restore.Weapons.Num());
}

bool FLambdaSaveGame::HasPendingRestore()
{
	return GPending.bArmed;
}

TArray<FLambdaSaveInfo> FLambdaSaveGame::List()
{
	TArray<FLambdaSaveInfo> Out;
	const FString Dir = SaveDirectory();
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.sav")), /*Files=*/ true, /*Directories=*/ false);
	for (const FString& File : Files)
	{
		FLambdaSaveInfo Info;
		Info.Name = FPaths::GetBaseFilename(File);
		Info.When = IFileManager::Get().GetTimeStamp(*(Dir / File));

		TArray<uint8> Bytes;
		FSourceKeyValues Root;
		if (FFileHelper::LoadFileToArray(Bytes, *(Dir / File)) && FSourceKeyValues::ParseSingle(Bytes, Root, nullptr))
		{
			Info.MapName = Root.GetString(TEXT("map"));
		}
		Info.Comment = FString::Printf(TEXT("%s  %s"), *Info.MapName, *Info.When.ToString(TEXT("%d/%m/%Y %H:%M")));
		Out.Add(Info);
	}
	// Newest first: that is the order a load dialog wants and the order "the last save" means.
	Out.Sort([](const FLambdaSaveInfo& A, const FLambdaSaveInfo& B) { return A.When > B.When; });
	return Out;
}

bool FLambdaSaveGame::HasAnySave()
{
	return List().Num() > 0;
}

FString FLambdaSaveGame::NewestSaveName()
{
	const TArray<FLambdaSaveInfo> Saves = List();
	return Saves.Num() > 0 ? Saves[0].Name : FString();
}
