#include "Entities/SourcePropData.h"
#include "FileSystem/LambdaFileSystem.h"
#include "Core/LambdaSourceModule.h"
#include "Formats/SourceKeyValues.h"

namespace
{
	const TCHAR* PropDataFile = TEXT("scripts/propdata.txt");
	/** How deep a chain of "base" entries is followed before it is called a loop. */
	constexpr int32 MaxBaseDepth = 16;
}

FSourcePropData& FSourcePropData::Get()
{
	static FSourcePropData Instance;
	return Instance;
}

void FSourcePropData::Load()
{
	Entries.Reset();
	BreakableChunks.Reset();
	bLoaded = false;

	TArray<uint8> Bytes;
	if (!FLambdaFileSystem::Get().ReadFile(PropDataFile, Bytes))
	{
		// ParsePropDataFile gives up quietly when the file is not there; props then have no health and cannot break.
		UE_LOG(LogLambdaSource, Log, TEXT("%s not found; props will not break"), PropDataFile);
		return;
	}

	FSourceKeyValues Root;
	FString Error;
	if (!FSourceKeyValues::ParseSingle(Bytes, Root, &Error))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("%s: %s"), PropDataFile, *Error);
		return;
	}

	for (const FSourceKeyValues& Section : Root.Children)
	{
		if (!Section.IsSection())
		{
			continue;
		}
		if (Section.Key.Equals(TEXT("BreakableModels"), ESearchCase::IgnoreCase))
		{
			// Each list is a set of chunk models, sorted smallest first.
			for (const FSourceKeyValues& ChunkType : Section.Children)
			{
				TArray<FString>& Models = BreakableChunks.Add(ChunkType.Key.ToLower());
				for (const FSourceKeyValues& Model : ChunkType.Children)
				{
					Models.Add(Model.Key.Replace(TEXT("\\"), TEXT("/")));
				}
			}
			continue;
		}
		Entries.Add(Section.Key.ToLower(), Section);
	}

	bLoaded = true;
	UE_LOG(LogLambdaSource, Log, TEXT("%s: %d prop types, %d breakable chunk lists"),
		PropDataFile, Entries.Num(), BreakableChunks.Num());
}

void FSourcePropData::ApplySection(const FSourceKeyValues& Section, FSourcePropDataEntry& Entry, int32 Depth) const
{
	if (Depth > MaxBaseDepth)
	{
		return;
	}

	// "Do we have a base?": the base is folded in first, so this section's own values override it.
	const FString Base = Section.GetString(TEXT("base"));
	if (!Base.IsEmpty())
	{
		if (const FSourceKeyValues* BaseSection = Entries.Find(Base.ToLower()))
		{
			ApplySection(*BaseSection, Entry, Depth + 1);
		}
	}

	// "Get damage modifiers, but only if they're specified, because our base may have already overridden them."
	Entry.DmgModBullet = Section.GetFloat(TEXT("dmg.bullets"), Entry.DmgModBullet);
	Entry.DmgModClub = Section.GetFloat(TEXT("dmg.club"), Entry.DmgModClub);
	Entry.DmgModExplosive = Section.GetFloat(TEXT("dmg.explosive"), Entry.DmgModExplosive);

	Entry.Health = Section.GetFloat(TEXT("health"), Entry.Health);
	Entry.ExplosiveDamage = Section.GetFloat(TEXT("explosive_damage"), Entry.ExplosiveDamage);
	Entry.ExplosiveRadius = Section.GetFloat(TEXT("explosive_radius"), Entry.ExplosiveRadius);

	Entry.BreakableModel = Section.GetString(TEXT("breakable_model"), Entry.BreakableModel);
	Entry.BreakableCount = Section.GetInt(TEXT("breakable_count"), Entry.BreakableCount);
	Entry.BreakableSkin = Section.GetInt(TEXT("breakable_skin"), Entry.BreakableSkin);
	Entry.bAllowStatic = Section.GetInt(TEXT("allowstatic"), Entry.bAllowStatic ? 1 : 0) != 0;
}

bool FSourcePropData::Resolve(const FString& EntryName, FSourcePropDataEntry& OutEntry) const
{
	const FSourceKeyValues* Section = EntryName.IsEmpty() ? nullptr : Entries.Find(EntryName.ToLower());
	if (!Section)
	{
		return false;
	}
	ApplySection(*Section, OutEntry, 0);
	return true;
}

bool FSourcePropData::ResolveForModel(const FString& ModelKeyValueText, FSourcePropDataEntry& OutEntry) const
{
	if (ModelKeyValueText.IsEmpty())
	{
		return false;
	}
	// CBaseProp::ParsePropData: the model's keyvalues hold a "prop_data" section, normally just naming a base.
	TArray<FSourceKeyValues> Roots;
	if (!FSourceKeyValues::ParseText(ModelKeyValueText, Roots))
	{
		return false;
	}
	for (const FSourceKeyValues& Root : Roots)
	{
		const FSourceKeyValues* PropSection = Root.Key.Equals(TEXT("prop_data"), ESearchCase::IgnoreCase)
			? &Root : Root.FindChild(TEXT("prop_data"));
		if (PropSection)
		{
			ApplySection(*PropSection, OutEntry, 0);
			return true;
		}
	}
	return false;
}

FString FSourcePropData::GetRandomChunkModel(const FString& ChunkType, int32 MaxSize) const
{
	const TArray<FString>* Models = ChunkType.IsEmpty() ? nullptr : BreakableChunks.Find(ChunkType.ToLower());
	if (!Models || Models->Num() == 0)
	{
		return FString();
	}
	// "Don't pick anything over the specified size": the lists run smallest to largest, so a small prop is held to
	// the front of its list.
	const int32 Last = MaxSize < 0 ? Models->Num() - 1 : FMath::Min(MaxSize, Models->Num() - 1);
	return (*Models)[FMath::RandRange(0, FMath::Max(0, Last))];
}
