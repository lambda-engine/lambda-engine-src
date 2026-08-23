#include "SourceWavFile.h"
#include "LambdaSourceModule.h"

namespace
{
	template <typename T>
	bool Read(TConstArrayView<uint8> Data, int64& Pos, T& Out)
	{
		if (Pos + (int64)sizeof(T) > Data.Num())
		{
			return false;
		}
		FMemory::Memcpy(&Out, Data.GetData() + Pos, sizeof(T));
		Pos += sizeof(T);
		return true;
	}

	constexpr uint16 WAVE_FORMAT_PCM = 0x0001;
	constexpr uint16 WAVE_FORMAT_EXTENSIBLE = 0xFFFE;
}

bool SourceWav::Parse(TConstArrayView<uint8> FileBytes, FSourceWavData& Out, FString* OutError)
{
	auto Fail = [&](const FString& Msg)
	{
		if (OutError) { *OutError = Msg; }
		return false;
	};

	Out = FSourceWavData();

	if (FileBytes.Num() < 12)
	{
		return Fail(TEXT("File too small to be a WAV"));
	}
	if (FMemory::Memcmp(FileBytes.GetData(), "RIFF", 4) != 0 || FMemory::Memcmp(FileBytes.GetData() + 8, "WAVE", 4) != 0)
	{
		return Fail(TEXT("Not a RIFF/WAVE file"));
	}

	uint16 FormatTag = 0;
	uint16 BitsPerSample = 0;
	int64 Pos = 12;
	TConstArrayView<uint8> RawData;

	while (Pos + 8 <= FileBytes.Num())
	{
		char ChunkId[4];
		FMemory::Memcpy(ChunkId, FileBytes.GetData() + Pos, 4);
		Pos += 4;
		uint32 ChunkSize = 0;
		if (!Read(FileBytes, Pos, ChunkSize))
		{
			break;
		}
		const int64 ChunkStart = Pos;
		const int64 ChunkEnd = FMath::Min<int64>(FileBytes.Num(), ChunkStart + ChunkSize);

		if (FMemory::Memcmp(ChunkId, "fmt ", 4) == 0)
		{
			int64 P = ChunkStart;
			uint16 Channels = 0;
			uint32 SampleRate = 0, ByteRate = 0;
			uint16 BlockAlign = 0;
			if (!Read(FileBytes, P, FormatTag) || !Read(FileBytes, P, Channels) || !Read(FileBytes, P, SampleRate) ||
				!Read(FileBytes, P, ByteRate) || !Read(FileBytes, P, BlockAlign) || !Read(FileBytes, P, BitsPerSample))
			{
				return Fail(TEXT("Truncated fmt chunk"));
			}
			Out.NumChannels = Channels;
			Out.SampleRate = (int32)SampleRate;
		}
		else if (FMemory::Memcmp(ChunkId, "data", 4) == 0)
		{
			RawData = TConstArrayView<uint8>(FileBytes.GetData() + ChunkStart, (int32)(ChunkEnd - ChunkStart));
		}

		// Chunks are word aligned.
		Pos = ChunkEnd + (ChunkSize & 1);
	}

	if (Out.NumChannels <= 0 || Out.SampleRate <= 0)
	{
		return Fail(TEXT("WAV has no usable fmt chunk"));
	}
	if (RawData.Num() == 0)
	{
		return Fail(TEXT("WAV has no data chunk"));
	}
	if (FormatTag != WAVE_FORMAT_PCM && FormatTag != WAVE_FORMAT_EXTENSIBLE)
	{
		return Fail(FString::Printf(TEXT("Unsupported WAV format tag 0x%04x (only uncompressed PCM is supported)"), FormatTag));
	}

	if (BitsPerSample == 16)
	{
		Out.Pcm16.Append(RawData.GetData(), RawData.Num());
	}
	else if (BitsPerSample == 8)
	{
		// 8-bit WAV samples are unsigned; convert to signed 16-bit.
		const int32 NumSamples = RawData.Num();
		Out.Pcm16.SetNumUninitialized(NumSamples * 2);
		int16* Dst = (int16*)Out.Pcm16.GetData();
		for (int32 i = 0; i < NumSamples; ++i)
		{
			Dst[i] = (int16)(((int32)RawData[i] - 128) << 8);
		}
	}
	else
	{
		return Fail(FString::Printf(TEXT("Unsupported WAV bit depth %d"), BitsPerSample));
	}

	return true;
}
