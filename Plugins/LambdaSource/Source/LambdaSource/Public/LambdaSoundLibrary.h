#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundWaveProcedural.h"
#include "SourceWavFile.h"
#include "LambdaSoundLibrary.generated.h"

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

	virtual int32 OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples) override;

	/** True once a non-looping sound has played all of its data. */
	bool IsFinished() const { return bFinished; }

private:
	TArray<uint8> Pcm;
	int32 Cursor = 0;
	bool bLoop = false;
	bool bFinished = false;
};

/**
 * Loads and caches sounds by their Source-relative name ("doors/door1_move.wav"), resolved through the virtual file
 * system as "sound/<name>". The decoded PCM is cached; a fresh wave object is handed out per play so that two
 * simultaneous plays of the same sound do not share a read cursor.
 */
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

	void Clear() { Cache.Reset(); }

private:
	FLambdaSoundCache() = default;

	TMap<FString, TSharedPtr<FSourceWavData>> Cache;
};
