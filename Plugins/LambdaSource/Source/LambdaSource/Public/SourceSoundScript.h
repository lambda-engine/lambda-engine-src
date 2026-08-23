#pragma once

#include "CoreMinimal.h"

/**
 * One entry from a game_sounds*.txt soundscript, e.g.
 *
 *   "Buttons.snd1"
 *   {
 *       "channel"    "CHAN_ITEM"
 *       "volume"     "0.7"
 *       "soundlevel" "SNDLVL_75dB"
 *       "pitch"      "100"
 *       "wave"       "buttons/button1.wav"
 *   }
 *
 * "rndwave" blocks list several waves; Source picks one at random per play.
 */
struct LAMBDASOURCE_API FSourceSoundScriptEntry
{
	FString Name;
	TArray<FString> Waves;		// relative to sound/, decorator characters already stripped
	float VolumeMin = 1.0f;
	float VolumeMax = 1.0f;
	float PitchMin = 100.0f;
	float PitchMax = 100.0f;
	float SoundLevel = 75.0f;	// dB; SNDLVL_NORM is 75

	/** Picks one of the waves at random, as Source does for rndwave. */
	const FString* PickWave() const;
	float PickVolume() const { return (VolumeMin == VolumeMax) ? VolumeMin : FMath::FRandRange(VolumeMin, VolumeMax); }
	float PickPitch() const { return (PitchMin == PitchMax) ? PitchMin : FMath::FRandRange(PitchMin, PitchMax); }
};

/**
 * Loads the soundscripts listed by scripts/game_sounds_manifest.txt so entities can refer to sounds by script name
 * ("Buttons.snd1", "Doors.FullClose1") instead of a wav path, exactly like Source.
 */
class LAMBDASOURCE_API FSourceSoundScripts
{
public:
	static FSourceSoundScripts& Get();

	/** Parses the manifest and every script it lists. Safe to call repeatedly; only the first call does work. */
	void Initialize();
	/** Drops everything so the next Initialize() re-reads (used when a new map/game directory is mounted). */
	void Reset();

	/** Looks up a script entry by name (case-insensitive), or null. */
	const FSourceSoundScriptEntry* Find(const FString& ScriptName);

	int32 Num() const { return Entries.Num(); }

	/** Strips Source's leading wave decorator characters (soundchars.h: '*', ')', '#', '^', ...). */
	static FString StripSoundChars(const FString& WaveName);

private:
	FSourceSoundScripts() = default;
	void LoadScriptFile(const FString& RelativePath);

	TMap<FString, FSourceSoundScriptEntry> Entries;
	bool bInitialized = false;
};
