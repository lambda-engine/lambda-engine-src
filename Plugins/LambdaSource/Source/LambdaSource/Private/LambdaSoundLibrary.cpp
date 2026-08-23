#include "LambdaSoundLibrary.h"
#include "LambdaFileSystem.h"
#include "LambdaSourceModule.h"
#include "SourceSoundScript.h"

// ---------------------------------------------------------------------------------------------------------------------
// ULambdaSoundWave
// ---------------------------------------------------------------------------------------------------------------------

void ULambdaSoundWave::InitFromWav(const FSourceWavData& Wav, bool bInLoop)
{
	Pcm = Wav.Pcm16;
	Cursor = 0;
	bLoop = bInLoop;
	bFinished = false;

	SetSampleRate(Wav.SampleRate);
	NumChannels = Wav.NumChannels;
	Duration = bInLoop ? INDEFINITELY_LOOPING_DURATION : Wav.GetDuration();
	SoundGroup = SOUNDGROUP_Effects;
	bLooping = bInLoop;
}

int32 ULambdaSoundWave::OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples)
{
	// The caller wants NumSamples int16 samples written into OutAudio.
	const int32 BytesNeeded = NumSamples * sizeof(int16);
	OutAudio.Reset();
	OutAudio.AddUninitialized(BytesNeeded);
	uint8* Dst = OutAudio.GetData();

	int32 BytesWritten = 0;
	while (BytesWritten < BytesNeeded && Pcm.Num() > 0)
	{
		if (Cursor >= Pcm.Num())
		{
			if (!bLoop)
			{
				break;
			}
			Cursor = 0;
		}
		const int32 Chunk = FMath::Min(BytesNeeded - BytesWritten, Pcm.Num() - Cursor);
		FMemory::Memcpy(Dst + BytesWritten, Pcm.GetData() + Cursor, Chunk);
		BytesWritten += Chunk;
		Cursor += Chunk;
	}

	if (BytesWritten < BytesNeeded)
	{
		// Ran out of data on a one-shot: pad with silence and mark ourselves done.
		FMemory::Memzero(Dst + BytesWritten, BytesNeeded - BytesWritten);
		bFinished = true;
	}
	return NumSamples;
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
