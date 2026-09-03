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
#include "World/SourceEntity.h"
#include "Creatures/SourceGameNPC.h"
#include "Creatures/SourceNPCBase.h"
#include "Entities/SourceGamePointEntity.h"
#include "Entities/SourceGameEntity.h"
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

	/**
	 * ISaveState over a flat list of key/value pairs - the concrete thing an entity writes itself into.
	 *
	 * One of these per saved entity, so keys are entity-local and two doors may both write "open".
	 */
	class FSaveStateKV : public lambda::ISaveState
	{
	public:
		TArray<TPair<FString, FString>> Pairs;

		void WriteInt(const char* Key, int Value) override
		{
			Pairs.Emplace(ANSI_TO_TCHAR(Key), FString::FromInt(Value));
		}
		void WriteFloat(const char* Key, float Value) override
		{
			Pairs.Emplace(ANSI_TO_TCHAR(Key), FString::SanitizeFloat(Value));
		}
		void WriteString(const char* Key, const char* Value) override
		{
			Pairs.Emplace(ANSI_TO_TCHAR(Key), ANSI_TO_TCHAR(Value ? Value : ""));
		}
		void WriteVec3(const char* Key, const lambda::Vec3& Value) override
		{
			Pairs.Emplace(ANSI_TO_TCHAR(Key), FString::Printf(TEXT("%.2f %.2f %.2f"), Value.x, Value.y, Value.z));
		}

		const FString* Find(const char* Key) const
		{
			const FString Wanted(ANSI_TO_TCHAR(Key));
			for (const TPair<FString, FString>& Pair : Pairs)
			{
				if (Pair.Key.Equals(Wanted, ESearchCase::IgnoreCase))
				{
					return &Pair.Value;
				}
			}
			return nullptr;
		}
		bool Has(const char* Key) const override { return Find(Key) != nullptr; }
		int ReadInt(const char* Key, int Default) const override
		{
			const FString* V = Find(Key);
			return V ? FCString::Atoi(**V) : Default;
		}
		float ReadFloat(const char* Key, float Default) const override
		{
			const FString* V = Find(Key);
			return V ? FCString::Atof(**V) : Default;
		}
		const char* ReadString(const char* Key, const char* Default) const override
		{
			const FString* V = Find(Key);
			if (!V)
			{
				return Default;
			}
			const FTCHARToUTF8 Converted(**V);
			Returned.SetNumUninitialized(Converted.Length() + 1);
			FMemory::Memcpy(Returned.GetData(), Converted.Get(), Converted.Length());
			Returned[Converted.Length()] = 0;
			return Returned.GetData();
		}
		lambda::Vec3 ReadVec3(const char* Key, const lambda::Vec3& Default) const override
		{
			const FString* V = Find(Key);
			if (!V)
			{
				return Default;
			}
			FVector3f Parsed = FVector3f::ZeroVector;
			FSourceCoords::ParseVector(*V, Parsed);
			return lambda::Vec3{ Parsed.X, Parsed.Y, Parsed.Z };
		}

	private:
		mutable TArray<ANSICHAR> Returned;
	};

	/** Everything a saved entity carries, keyed by its place in the map's spawn order. */
	struct FEntitySave
	{
		int32 Index = INDEX_NONE;
		FString ClassName;
		FVector3f Origin = FVector3f::ZeroVector;
		FVector3f Angles = FVector3f::ZeroVector;
		float Health = -1.0f;			// NPCs only; -1 means "not a thing with health"
		bool bDead = false;
		FSaveStateKV Fields;			// whatever the entity's own behaviour wrote
	};

	TArray<FEntitySave> GPendingEntities;

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

	// Every entity the map spawned, by its place in the spawn order. That order is a property of the BSP, so
	// it is the same on the next load of the same map - which is what lets a restore find each one again
	// without needing an id the map never promised to give us.
	if (ASourceBSPWorldActor* BSP = Cast<ASourceBSPWorldActor>(
		UGameplayStatics::GetActorOfClass(World, ASourceBSPWorldActor::StaticClass())))
	{
		Lines.Add(TEXT("\t\"entities\""));
		Lines.Add(TEXT("\t{"));
		for (int32 i = 0; i < BSP->SpawnedActors.Num(); ++i)
		{
			AActor* Actor = BSP->SpawnedActors[i].Get();
			if (!IsValid(Actor))
			{
				continue;
			}
			FSaveStateKV Fields;
			FString ClassName;
			float EntHealth = -1.0f;

			if (ASourceGameEntity* GameEnt = Cast<ASourceGameEntity>(Actor))
			{
				ClassName = GameEnt->GetEntity().ClassName;
				if (GameEnt->GetBehaviour()) { GameEnt->GetBehaviour()->SaveState(Fields); }
			}
			else if (ASourceGamePointEntity* PointEnt = Cast<ASourceGamePointEntity>(Actor))
			{
				ClassName = PointEnt->GetEntity().ClassName;
				if (PointEnt->GetBehaviour()) { PointEnt->GetBehaviour()->SaveState(Fields); }
			}
			else if (ASourceNPCBase* NPC = Cast<ASourceNPCBase>(Actor))
			{
				ClassName = NPC->GetSourceEntity().ClassName;
				EntHealth = NPC->GetHealth();
				if (ASourceGameNPC* GameNPC = Cast<ASourceGameNPC>(Actor))
				{
					if (GameNPC->GetBehaviour()) { GameNPC->GetBehaviour()->SaveState(Fields); }
				}
			}
			else if (const ASourceEntity* Ent = Cast<ASourceEntity>(Actor))
			{
				ClassName = Ent->GetEntity().ClassName;
			}
			else
			{
				continue;	// not something the map named; nothing to put back
			}

			const FVector3f EntOrigin = FSourceCoords::ToSource(Actor->GetActorLocation(), Scale);
			const FVector3f EntAngles = FSourceCoords::AnglesFromUE(Actor->GetActorRotation());
			Lines.Add(FString::Printf(TEXT("\t\t\"%d\""), i));
			Lines.Add(TEXT("\t\t{"));
			Lines.Add(FString::Printf(TEXT("\t\t\t\"classname\"\t\"%s\""), *ClassName));
			Lines.Add(FString::Printf(TEXT("\t\t\t\"origin\"\t\"%.2f %.2f %.2f\""), EntOrigin.X, EntOrigin.Y, EntOrigin.Z));
			Lines.Add(FString::Printf(TEXT("\t\t\t\"angles\"\t\"%.2f %.2f %.2f\""), EntAngles.X, EntAngles.Y, EntAngles.Z));
			if (EntHealth >= 0.0f)
			{
				Lines.Add(FString::Printf(TEXT("\t\t\t\"health\"\t\"%.1f\""), EntHealth));
			}
			for (const TPair<FString, FString>& Pair : Fields.Pairs)
			{
				Lines.Add(FString::Printf(TEXT("\t\t\t\"%s\"\t\"%s\""), *Pair.Key, *Pair.Value));
			}
			Lines.Add(TEXT("\t\t}"));
		}
		Lines.Add(TEXT("\t}"));
	}
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

	// The world half of the save. Parsed here and held until the map's entities exist.
	GPendingEntities.Reset();
	if (const FSourceKeyValues* EntBlock = Root.FindChild(TEXT("entities")))
	{
		for (const FSourceKeyValues& Child : EntBlock->Children)
		{
			if (!Child.IsSection())
			{
				continue;
			}
			FEntitySave Ent;
			Ent.Index = FCString::Atoi(*Child.Key);
			for (const FSourceKeyValues& Field : Child.Children)
			{
				if (Field.Key.Equals(TEXT("classname"), ESearchCase::IgnoreCase)) { Ent.ClassName = Field.Value; }
				else if (Field.Key.Equals(TEXT("origin"), ESearchCase::IgnoreCase)) { FSourceCoords::ParseVector(Field.Value, Ent.Origin); }
				else if (Field.Key.Equals(TEXT("angles"), ESearchCase::IgnoreCase)) { FSourceCoords::ParseVector(Field.Value, Ent.Angles); }
				else if (Field.Key.Equals(TEXT("health"), ESearchCase::IgnoreCase)) { Ent.Health = FCString::Atof(*Field.Value); }
				else
				{
					// Anything else belongs to the entity's own behaviour; it knows what its keys mean.
					Ent.Fields.Pairs.Emplace(Field.Key, Field.Value);
				}
			}
			GPendingEntities.Add(MoveTemp(Ent));
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

void FLambdaSaveGame::ApplyPendingWorldRestore(ASourceBSPWorldActor* WorldActor)
{
	if (GPendingEntities.Num() == 0 || !WorldActor)
	{
		return;
	}
	const TArray<FEntitySave> Entities = MoveTemp(GPendingEntities);
	GPendingEntities.Reset();

	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	int32 Restored = 0;
	int32 Removed = 0;
	for (const FEntitySave& Ent : Entities)
	{
		if (!WorldActor->SpawnedActors.IsValidIndex(Ent.Index))
		{
			continue;	// the map has changed under the save; skip rather than put state on a stranger
		}
		AActor* Actor = WorldActor->SpawnedActors[Ent.Index].Get();
		if (!IsValid(Actor))
		{
			continue;
		}

		// The spawn order is stable for a given BSP, but a recompiled map can reorder it. Checking the
		// classname turns "the map changed" from a corrupted restore into a skipped entity.
		FString ActualClass;
		if (const ASourceEntity* AsEnt = Cast<ASourceEntity>(Actor)) { ActualClass = AsEnt->GetEntity().ClassName; }
		else if (const ASourceNPCBase* AsNPC = Cast<ASourceNPCBase>(Actor)) { ActualClass = AsNPC->GetSourceEntity().ClassName; }
		if (!ActualClass.Equals(Ent.ClassName, ESearchCase::IgnoreCase))
		{
			UE_LOG(LogLambda, Verbose, TEXT("restore: entity %d is '%s', save says '%s' - skipped"),
				Ent.Index, *ActualClass, *Ent.ClassName);
			continue;
		}

		// An NPC that was dead stays dead. Removing it is the honest version of restoring a corpse: the body
		// itself was a runtime ragdoll and is not in the save, so bringing the soldier back alive would be
		// worse than the room simply being empty where he fell.
		if (ASourceNPCBase* NPC = Cast<ASourceNPCBase>(Actor))
		{
			if (Ent.Health >= 0.0f && Ent.Health <= 0.0f)
			{
				Actor->Destroy();
				++Removed;
				continue;
			}
			if (Ent.Health > 0.0f)
			{
				NPC->SetHealth(Ent.Health);
			}
		}

		Actor->SetActorLocation(FSourceCoords::ToUE(Ent.Origin, Scale), false, nullptr, ETeleportType::TeleportPhysics);
		Actor->SetActorRotation(FSourceCoords::AnglesToUE(Ent.Angles));

		// And whatever the entity's own behaviour wrote down about itself.
		lambda::IEntity* Behaviour = nullptr;
		if (ASourceGameEntity* GameEnt = Cast<ASourceGameEntity>(Actor)) { Behaviour = GameEnt->GetBehaviour(); }
		else if (ASourceGamePointEntity* PointEnt = Cast<ASourceGamePointEntity>(Actor)) { Behaviour = PointEnt->GetBehaviour(); }
		else if (ASourceGameNPC* GameNPC = Cast<ASourceGameNPC>(Actor)) { Behaviour = GameNPC->GetBehaviour(); }
		if (Behaviour)
		{
			Behaviour->RestoreState(Ent.Fields);
		}
		++Restored;
	}
	UE_LOG(LogLambda, Log, TEXT("restored %d entities, removed %d that were dead"), Restored, Removed);
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
