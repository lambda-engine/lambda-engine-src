#pragma once

#include "CoreMinimal.h"

/** Decoded contents of a RIFF/WAVE file: 16-bit signed PCM, interleaved. */
struct LAMBDASOURCE_API FSourceWavData
{
	TArray<uint8> Pcm16;		// interleaved signed 16-bit samples
	int32 SampleRate = 0;
	int32 NumChannels = 0;

	int32 NumFrames() const { return (NumChannels > 0) ? (Pcm16.Num() / (2 * NumChannels)) : 0; }
	float GetDuration() const { return (SampleRate > 0) ? ((float)NumFrames() / (float)SampleRate) : 0.0f; }
	bool IsValid() const { return Pcm16.Num() > 0 && SampleRate > 0 && NumChannels > 0; }
};

/**
 * Minimal RIFF/WAVE reader for the sounds shipped with Source games. Handles uncompressed PCM (8- and 16-bit), which
 * is what the stock HL2 wavs use; compressed formats (ADPCM, xWMA) are reported as unsupported rather than guessed at.
 */
namespace SourceWav
{
	LAMBDASOURCE_API bool Parse(TConstArrayView<uint8> FileBytes, FSourceWavData& Out, FString* OutError = nullptr);
}
