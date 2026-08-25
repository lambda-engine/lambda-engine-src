#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundWaveProcedural.h"
#include "Sound/SoundGenerator.h"
#include "Formats/SourceWavFile.h"
#include "LambdaSoundLibrary.generated.h"

/**
 * Plays decoded Source PCM and, crucially, reports when it has run out.
 *
 * The audio mixer only ever ends a procedural voice when its ISoundGenerator says IsFinished(); the return value
 * of OnGeneratePCMAudio is not consulted for that. A wave without a generator therefore holds its voice for the
 * lifetime of the game, and once every channel is held no further sound can start - which is what made rapid
 * firing go silent after a few dozen shots.
 */
class LAMBDASOURCE_API FLambdaSoundGenerator : public ISoundGenerator
{
public:
	FLambdaSoundGenerator(TArray<uint8> InPcm, int32 InNumChannels, bool bInLoop);

	virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override;
	virtual bool IsFinished() const override { return bFinished; }
	virtual int32 GetNumChannels() const override { return NumChannels; }

private:
	TArray<uint8> Pcm;		// interleaved int16
	int32 Cursor = 0;
	int32 NumChannels = 1;
	bool bLoop = false;
	bool bFinished = false;
};

/**
 * A procedural sound wave fed from PCM decoded out of a Source .wav (loose or inside a mounted VPK), so game sounds
 * can be played at runtime without importing them as assets.
 */
UCLASS()
class LAMBDASOURCE_API ULambdaSoundWave : public USoundWaveProcedural
{
	GENERATED_BODY()

public:
	/** Fills in the wave from decoded PCM. */
	void InitFromWav(const FSourceWavData& Wav, bool bInLoop);

	virtual ISoundGeneratorPtr CreateSoundGenerator(const FSoundGeneratorInitParams& InParams) override;

private:
	TArray<uint8> Pcm;
	bool bLoop = false;
};

/**
 * Loads and caches sounds by their Source-relative name ("doors/door1_move.wav"), resolved through the virtual file
 * system as "sound/<name>". The decoded PCM is cached; a fresh wave object is handed out per play so that two
 * simultaneous plays of the same sound do not share a read cursor.
 */
class USoundAttenuation;

class LAMBDASOURCE_API FLambdaSoundCache
{
public:
	static FLambdaSoundCache& Get();

	/** Returns a wave ready to play, or null if the sound is missing/unsupported. Outer owns the wave's lifetime. */
	ULambdaSoundWave* CreateWave(UObject* Outer, const FString& SoundName, bool bLoop);

	/**
	 * Resolves an entity's sound field, which may be either a direct wav path ("doors/door1_move.wav") or a
	 * soundscript name ("Buttons.snd1"), and returns a wave plus the script's volume/pitch.
	 */
	ULambdaSoundWave* CreateWaveResolved(UObject* Outer, const FString& SoundName, bool bLoop,
		float& OutVolume, float& OutPitch);

	/** Decoded PCM for a sound name, cached. Returns null if it could not be loaded. */
	const FSourceWavData* GetWavData(const FString& SoundName);

	/**
	 * The falloff a soundscript's "soundlevel" asks for. Source states it as the sound's level in dB at a foot
	 * (SNDLVL_NORM is 75) and turns it into an attenuation, ATTN = 20 / (dB - 50); the louder the sound, the
	 * further it carries. Without it every sound in the level is equally loud wherever it was made.
	 */
	USoundAttenuation* GetAttenuationForSoundLevel(float SoundLevelDb);

	void Clear() { Cache.Reset(); }

private:
	FLambdaSoundCache() = default;

	TMap<FString, TSharedPtr<FSourceWavData>> Cache;
	TMap<int32, TObjectPtr<USoundAttenuation>> AttenuationByLevel;
};
