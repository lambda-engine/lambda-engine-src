#include "Audio/LambdaSoundLibrary.h"
#include "Sound/SoundAttenuation.h"
#include "Core/LambdaSourceSettings.h"
#include "FileSystem/LambdaFileSystem.h"
#include "Core/LambdaSourceModule.h"
#include "Audio/SourceSoundScript.h"

// ---------------------------------------------------------------------------------------------------------------------
// ULambdaSoundWave
// ---------------------------------------------------------------------------------------------------------------------

FLambdaSoundGenerator::FLambdaSoundGenerator(TArray<uint8> InPcm, int32 InNumChannels, bool bInLoop)
	: Pcm(MoveTemp(InPcm))
	, NumChannels(FMath::Max(1, InNumChannels))
	, bLoop(bInLoop)
{
}

int32 FLambdaSoundGenerator::OnGenerateAudio(float* OutAudio, int32 NumSamples)
{
	// The mixer wants float samples; the decoded Source wav is interleaved int16.
	int32 Written = 0;
	while (Written < NumSamples && Pcm.Num() >= 2)
	{
		if (Cursor + 1 >= Pcm.Num())
		{
			if (!bLoop)
			{
				break;
			}
			Cursor = 0;
		}
		const int16 Sample = (int16)((uint16)Pcm[Cursor] | ((uint16)Pcm[Cursor + 1] << 8));
		OutAudio[Written++] = (float)Sample / 32768.0f;
		Cursor += 2;
	}

	if (Written < NumSamples)
	{
		// Out of data on a one-shot. Report it so the mixer releases the voice instead of holding it forever.
		bFinished = true;
	}
	return Written;
}

void ULambdaSoundWave::InitFromWav(const FSourceWavData& Wav, bool bInLoop)
{
	Pcm = Wav.Pcm16;
	bLoop = bInLoop;

	SetSampleRate(Wav.SampleRate);
	NumChannels = Wav.NumChannels;
	Duration = bInLoop ? INDEFINITELY_LOOPING_DURATION : Wav.GetDuration();
	SoundGroup = SOUNDGROUP_Effects;
	bLooping = bInLoop;
}

ISoundGeneratorPtr ULambdaSoundWave::CreateSoundGenerator(const FSoundGeneratorInitParams& InParams)
{
	// A generator per playback, so two overlapping plays of the same sound do not share a read cursor.
	return ISoundGeneratorPtr(new FLambdaSoundGenerator(Pcm, NumChannels, bLoop));
}

// ---------------------------------------------------------------------------------------------------------------------
// FLambdaSoundCache
// ---------------------------------------------------------------------------------------------------------------------

FLambdaSoundCache& FLambdaSoundCache::Get()
{
	static FLambdaSoundCache Instance;
	return Instance;
}

const FSourceWavData* FLambdaSoundCache::GetWavData(const FString& SoundName)
{
	FString Key = FLambdaFileSystem::NormalizeRelativePath(SoundName).ToLower();
	if (Key.IsEmpty())
	{
		return nullptr;
	}
	if (const TSharedPtr<FSourceWavData>* Found = Cache.Find(Key))
	{
		return Found->IsValid() ? Found->Get() : nullptr;
	}

	// Source sound names are relative to the game's "sound" folder.
	FString RelPath = Key;
	if (!RelPath.StartsWith(TEXT("sound/")))
	{
		RelPath = TEXT("sound/") + RelPath;
	}

	TArray<uint8> Bytes;
	if (!FLambdaFileSystem::Get().ReadFile(RelPath, Bytes))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Sound not found: %s"), *RelPath);
		Cache.Add(Key, nullptr);
		return nullptr;
	}

	TSharedPtr<FSourceWavData> Wav = MakeShared<FSourceWavData>();
	FString Error;
	if (!SourceWav::Parse(Bytes, *Wav, &Error) || !Wav->IsValid())
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Sound '%s': %s"), *RelPath, *Error);
		Cache.Add(Key, nullptr);
		return nullptr;
	}

	UE_LOG(LogLambdaSource, Verbose, TEXT("Sound '%s': %d Hz, %d ch, %.2fs"), *RelPath, Wav->SampleRate, Wav->NumChannels, Wav->GetDuration());
	Cache.Add(Key, Wav);
	return Wav.Get();
}

ULambdaSoundWave* FLambdaSoundCache::CreateWave(UObject* Outer, const FString& SoundName, bool bLoop)
{
	const FSourceWavData* Wav = GetWavData(SoundName);
	if (!Wav)
	{
		return nullptr;
	}
	ULambdaSoundWave* Wave = NewObject<ULambdaSoundWave>(Outer ? Outer : (UObject*)GetTransientPackage());
	Wave->InitFromWav(*Wav, bLoop);
	return Wave;
}

ULambdaSoundWave* FLambdaSoundCache::CreateWaveResolved(UObject* Outer, const FString& SoundName, bool bLoop,
	float& OutVolume, float& OutPitch)
{
	OutVolume = 1.0f;
	OutPitch = 1.0f;

	if (SoundName.IsEmpty() || SoundName == TEXT("0"))
	{
		return nullptr;
	}

	// A name that is not a wav path is a soundscript entry (Source resolves these through the sound emitter system).
	if (!SoundName.EndsWith(TEXT(".wav"), ESearchCase::IgnoreCase))
	{
		const FSourceSoundScriptEntry* Entry = FSourceSoundScripts::Get().Find(SoundName);
		if (!Entry)
		{
			UE_LOG(LogLambdaSource, Warning, TEXT("Unknown soundscript '%s'"), *SoundName);
			return nullptr;
		}
		const FString* Wave = Entry->PickWave();
		if (!Wave)
		{
			return nullptr;
		}
		OutVolume = Entry->PickVolume();
		OutPitch = Entry->PickPitch() / 100.0f;
		return CreateWave(Outer, *Wave, bLoop);
	}

	return CreateWave(Outer, SoundName, bLoop);
}

USoundAttenuation* FLambdaSoundCache::GetAttenuationForSoundLevel(float SoundLevelDb)
{
	// SNDLVL_TO_ATTN: attenuation = 20 / (dB - 50) above 50 dB, and Source's mixer keeps a sound audible to
	// roughly 1000 units / attenuation. Below that it is an unattenuated, everywhere sound (SNDLVL_NONE).
	const int32 Key = FMath::RoundToInt(SoundLevelDb);
	if (TObjectPtr<USoundAttenuation>* Found = AttenuationByLevel.Find(Key))
	{
		return Found->Get();
	}
	USoundAttenuation* Attenuation = nullptr;
	if (SoundLevelDb > 50.0f)
	{
		const float Attn = 20.0f / (SoundLevelDb - 50.0f);
		const float FalloffUnits = 1000.0f / FMath::Max(Attn, 0.01f);
		const float Scale = ULambdaSourceSettings::Get().UnitScale;
		Attenuation = NewObject<USoundAttenuation>(GetTransientPackage());
		Attenuation->AddToRoot();
		FSoundAttenuationSettings& Settings = Attenuation->Attenuation;
		Settings.bAttenuate = true;
		Settings.bSpatialize = true;
		Settings.AttenuationShape = EAttenuationShape::Sphere;
		Settings.DistanceAlgorithm = EAttenuationDistanceModel::NaturalSound;
		Settings.dBAttenuationAtMax = -60.0f;
		// The radius Source treats as "full volume" is small; everything past it rolls off to the falloff distance.
		Settings.AttenuationShapeExtents = FVector(100.0f * Scale, 0.0f, 0.0f);
		Settings.FalloffDistance = FalloffUnits * Scale;
	}
	AttenuationByLevel.Add(Key, Attenuation);
	return Attenuation;
}
