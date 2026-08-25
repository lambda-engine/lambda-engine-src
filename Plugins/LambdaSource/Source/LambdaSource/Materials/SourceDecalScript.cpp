#include "Materials/SourceDecalScript.h"
#include "FileSystem/LambdaFileSystem.h"
#include "Core/LambdaSourceModule.h"
#include "Formats/SourceKeyValues.h"

namespace
{
	// DECAL_LIST_FILE / TRANSLATION_DATA_SECTION in game/shared/decals.cpp.
	const TCHAR* DecalListFile = TEXT("scripts/decals_subrect.txt");
	const TCHAR* TranslationDataSection = TEXT("TranslationData");
	const TCHAR* ImpactConcrete = TEXT("Impact.Concrete");
}

FSourceDecalScript& FSourceDecalScript::Get()
{
	static FSourceDecalScript Instance;
	return Instance;
}

void FSourceDecalScript::Reset()
{
	Groups.Reset();
	GameMaterialTranslation.Reset();
	bInitialized = false;
}

void FSourceDecalScript::Initialize()
{
	if (bInitialized)
	{
		return;
	}
	bInitialized = true;

	TArray<uint8> Bytes;
	if (!FLambdaFileSystem::Get().ReadFile(DecalListFile, Bytes))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Decal script not found: %s"), DecalListFile);
		return;
	}

	TArray<FSourceKeyValues> Roots;
	FString Error;
	if (!FSourceKeyValues::ParseText(FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num())), Roots, &Error))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("%s: %s"), DecalListFile, *Error);
		return;
	}

	const FSourceKeyValues* Translation = nullptr;
	for (const FSourceKeyValues& Root : Roots)
	{
		if (!Root.IsSection() || Root.Children.Num() == 0)
		{
			continue;
		}
		if (Root.Key.Equals(TranslationDataSection, ESearchCase::IgnoreCase))
		{
			Translation = &Root;
			continue;
		}

		TArray<FSourceDecalEntry> Entries;
		for (const FSourceKeyValues& Child : Root.Children)
		{
			FSourceDecalEntry Entry;
			Entry.MaterialName = Child.Key;
			Entry.Weight = FCString::Atof(*Child.Value);
			if (Entry.Weight <= 0.0f)
			{
				Entry.Weight = 1.0f;
			}
			Entries.Add(MoveTemp(Entry));
		}
		Groups.Add(Root.Key.ToLower(), MoveTemp(Entries));
	}

	if (Translation)
	{
		for (const FSourceKeyValues& Child : Translation->Children)
		{
			// The key is the single game material character; an empty value means "no decal on this surface".
			if (Child.Key.IsEmpty() || Child.Value.IsEmpty())
			{
				continue;
			}
			if (!Groups.Contains(Child.Value.ToLower()))
			{
				UE_LOG(LogLambdaSource, Warning, TEXT("%s: game material '%s' references unknown decal group '%s'"),
					DecalListFile, *Child.Key, *Child.Value);
				continue;
			}
			GameMaterialTranslation.Add(FChar::ToUpper(Child.Key[0]), Child.Value);
		}
	}
	else
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("%s: missing section '%s'"), DecalListFile, TranslationDataSection);
	}

	UE_LOG(LogLambdaSource, Log, TEXT("Decal script: %d groups, %d game material translations"),
		Groups.Num(), GameMaterialTranslation.Num());
}

FString FSourceDecalScript::TranslateDecalForGameMaterial(const FString& DecalName, TCHAR GameMaterial) const
{
	// CDecalEmitterSystem::TranslateDecalForGameMaterial
	if (GameMaterial == TEXT('C'))
	{
		return DecalName;
	}
	if (DecalName.Equals(ImpactConcrete, ESearchCase::IgnoreCase))
	{
		if (GameMaterial == TEXT('-'))
		{
			return FString();
		}
		if (const FString* Translated = GameMaterialTranslation.Find(GameMaterial))
		{
			return *Translated;
		}
	}
	return DecalName;
}

FString FSourceDecalScript::PickDecalMaterial(const FString& GroupName) const
{
	const TArray<FSourceDecalEntry>* Entries = Groups.Find(GroupName.ToLower());
	if (!Entries || Entries->Num() == 0)
	{
		return FString();
	}

	// The same running-total weighted pick CDecalEmitterSystem::GetDecalIndexForName uses.
	float TotalWeight = 0.0f;
	const FSourceDecalEntry* Chosen = nullptr;
	for (const FSourceDecalEntry& Entry : *Entries)
	{
		TotalWeight += Entry.Weight;
		if (!Chosen || FMath::FRandRange(0.0f, TotalWeight) < Entry.Weight)
		{
			Chosen = &Entry;
		}
	}
	return Chosen ? Chosen->MaterialName : FString();
}

FString FSourceDecalScript::GetImpactDecalMaterial(TCHAR GameMaterial) const
{
	// GetImpactDecal in fx_impact.cpp: start from Impact.Concrete, let the game material redirect it.
	const FString Group = TranslateDecalForGameMaterial(ImpactConcrete, GameMaterial);
	return Group.IsEmpty() ? FString() : PickDecalMaterial(Group);
}

void FSourceDecalScript::GetAllDecalMaterials(TArray<FString>& OutNames) const
{
	for (const auto& Pair : Groups)
	{
		for (const FSourceDecalEntry& Entry : Pair.Value)
		{
			OutNames.AddUnique(Entry.MaterialName);
		}
	}
}
