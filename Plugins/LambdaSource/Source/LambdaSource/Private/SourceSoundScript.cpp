#include "SourceSoundScript.h"
#include "LambdaFileSystem.h"
#include "LambdaSourceModule.h"
#include "SourceKeyValues.h"

namespace
{
	/** Named volume/pitch constants used by soundscripts. */
	bool ParseNamedLevel(const FString& Text, float& Out)
	{
		if (Text.Equals(TEXT("VOL_NORM"), ESearchCase::IgnoreCase)) { Out = 1.0f; return true; }
		if (Text.Equals(TEXT("PITCH_NORM"), ESearchCase::IgnoreCase)) { Out = 100.0f; return true; }
		if (Text.Equals(TEXT("PITCH_LOW"), ESearchCase::IgnoreCase)) { Out = 95.0f; return true; }
		if (Text.Equals(TEXT("PITCH_HIGH"), ESearchCase::IgnoreCase)) { Out = 120.0f; return true; }
		return false;
	}

	/** Soundscript numbers may be a single value or a "min, max" range. */
	void ParseRange(const FString& Text, float& OutMin, float& OutMax, float Default)
	{
		OutMin = OutMax = Default;
		if (Text.IsEmpty())
		{
			return;
		}
		float Named = 0.0f;
		if (ParseNamedLevel(Text, Named))
		{
			OutMin = OutMax = Named;
			return;
		}
		FString Left, Right;
		if (Text.Split(TEXT(","), &Left, &Right))
		{
			Left.TrimStartAndEndInline();
			Right.TrimStartAndEndInline();
			if (!ParseNamedLevel(Left, OutMin)) { OutMin = FCString::Atof(*Left); }
			if (!ParseNamedLevel(Right, OutMax)) { OutMax = FCString::Atof(*Right); }
			if (OutMax < OutMin) { Swap(OutMin, OutMax); }
			return;
		}
		OutMin = OutMax = FCString::Atof(*Text);
	}

	/** "SNDLVL_75dB" -> 75, "SNDLVL_NORM" -> 75. */
	float ParseSoundLevel(const FString& Text)
	{
		if (Text.IsEmpty())
		{
			return 75.0f;
		}
		if (Text.Equals(TEXT("SNDLVL_NORM"), ESearchCase::IgnoreCase))
		{
			return 75.0f;
		}
		if (Text.Equals(TEXT("SNDLVL_NONE"), ESearchCase::IgnoreCase))
		{
			return 0.0f;
		}
		// SNDLVL_<n>dB
		FString Digits;
		for (TCHAR C : Text)
		{
			if (FChar::IsDigit(C)) { Digits.AppendChar(C); }
			else if (Digits.Len() > 0) { break; }
		}
		return Digits.IsEmpty() ? 75.0f : FCString::Atof(*Digits);
	}
}

const FString* FSourceSoundScriptEntry::PickWave() const
{
	if (Waves.Num() == 0)
	{
		return nullptr;
	}
	return &Waves[FMath::RandHelper(Waves.Num())];
}

FString FSourceSoundScripts::StripSoundChars(const FString& WaveName)
{
	// soundchars.h: the first couple of characters may be flags such as '*' (stream), ')' (spatial stereo),
	// '#' (dry mix), '^', '<', '>', '@', '~', '+', '(', '}', '$', '!', '?'.
	static const FString SoundChars = TEXT("*?!#><^@~+()}$");
	int32 Start = 0;
	while (Start < WaveName.Len() && SoundChars.Contains(FString::Chr(WaveName[Start])))
	{
		++Start;
	}
	return WaveName.RightChop(Start).TrimStartAndEnd();
}

FSourceSoundScripts& FSourceSoundScripts::Get()
{
	static FSourceSoundScripts Instance;
	return Instance;
}

void FSourceSoundScripts::Reset()
{
	Entries.Reset();
	bInitialized = false;
}

void FSourceSoundScripts::Initialize()
{
	if (bInitialized)
	{
		return;
	}
	bInitialized = true;

	// scripts/game_sounds_manifest.txt lists every soundscript to precache.
	TArray<uint8> ManifestBytes;
	if (!FLambdaFileSystem::Get().ReadFile(TEXT("scripts/game_sounds_manifest.txt"), ManifestBytes))
	{
		UE_LOG(LogLambdaSource, Log, TEXT("No scripts/game_sounds_manifest.txt found - entities can still name .wav files directly"));
		return;
	}

	FSourceKeyValues Manifest;
	FString Error;
	if (!FSourceKeyValues::ParseSingle(ManifestBytes, Manifest, &Error))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("game_sounds_manifest.txt: %s"), *Error);
		return;
	}

	int32 NumFiles = 0;
	for (const FSourceKeyValues& Child : Manifest.Children)
	{
		if (Child.Key.Equals(TEXT("precache_file"), ESearchCase::IgnoreCase) && !Child.Value.IsEmpty())
		{
			LoadScriptFile(Child.Value);
			++NumFiles;
		}
	}

	UE_LOG(LogLambdaSource, Log, TEXT("Soundscripts: %d entries from %d files"), Entries.Num(), NumFiles);
}

void FSourceSoundScripts::LoadScriptFile(const FString& RelativePath)
{
	TArray<uint8> Bytes;
	if (!FLambdaFileSystem::Get().ReadFile(RelativePath, Bytes))
	{
		// Manifests routinely list files a given game does not ship.
		UE_LOG(LogLambdaSource, Verbose, TEXT("Soundscript file not found: %s"), *RelativePath);
		return;
	}

	TArray<FSourceKeyValues> Roots;
	FString Error;
	if (!FSourceKeyValues::ParseBytes(Bytes, Roots, &Error))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Soundscript '%s': %s"), *RelativePath, *Error);
		return;
	}

	for (const FSourceKeyValues& Block : Roots)
	{
		if (!Block.IsSection() || Block.Key.IsEmpty())
		{
			continue;
		}

		FSourceSoundScriptEntry Entry;
		Entry.Name = Block.Key;

		ParseRange(Block.GetString(TEXT("volume")), Entry.VolumeMin, Entry.VolumeMax, 1.0f);
		ParseRange(Block.GetString(TEXT("pitch")), Entry.PitchMin, Entry.PitchMax, 100.0f);
		Entry.SoundLevel = ParseSoundLevel(Block.GetString(TEXT("soundlevel")));

		// A single "wave", or an "rndwave" block holding several.
		for (const FSourceKeyValues& Child : Block.Children)
		{
			if (Child.Key.Equals(TEXT("wave"), ESearchCase::IgnoreCase) && !Child.Value.IsEmpty())
			{
				Entry.Waves.Add(StripSoundChars(Child.Value));
			}
			else if (Child.Key.Equals(TEXT("rndwave"), ESearchCase::IgnoreCase) && Child.IsSection())
			{
				for (const FSourceKeyValues& Wave : Child.Children)
				{
					if (Wave.Key.Equals(TEXT("wave"), ESearchCase::IgnoreCase) && !Wave.Value.IsEmpty())
					{
						Entry.Waves.Add(StripSoundChars(Wave.Value));
					}
				}
			}
		}

		if (Entry.Waves.Num() > 0)
		{
			Entries.Add(Entry.Name.ToLower(), MoveTemp(Entry));
		}
	}
}

const FSourceSoundScriptEntry* FSourceSoundScripts::Find(const FString& ScriptName)
{
	Initialize();
	return Entries.Find(ScriptName.ToLower());
}
