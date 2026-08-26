#include "LambdaSuitVoice.h"

#include "LambdaEngine.h"

#include "Audio/LambdaSoundLibrary.h"
#include "Audio/SourceSentences.h"
#include "Formats/SourceWavFile.h"

#include "Kismet/GameplayStatics.h"

void FLambdaSuitVoice::Reset()
{
	Queue.Reset();
	SpeakingWords.Reset();
	WordIndex = 0;
	NextUpdateTime = 0.0f;
	NextWordTime = 0.0f;
	NoRepeatUntil.Reset();
}

void FLambdaSuitVoice::SetSuitUpdate(UObject* WorldContext, const FString& SentenceName, float NoRepeatSeconds)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (!World || SentenceName.IsEmpty())
	{
		return;
	}
	const float Now = World->GetTimeSeconds();

	// The no-repeat window. Source keeps a fixed table of 32 and gives up when it is full; a map is the same
	// idea without the ceiling, and the entries are cheap.
	if (const float* Until = NoRepeatUntil.Find(SentenceName))
	{
		if (Now < *Until)
		{
			return;
		}
	}
	if (NoRepeatSeconds > 0.0f)
	{
		NoRepeatUntil.Add(SentenceName, Now + NoRepeatSeconds);
	}

	// Already waiting to be said: do not stack it up twice.
	if (Queue.Contains(SentenceName))
	{
		return;
	}
	if (Queue.Num() >= MaxQueued)
	{
		Queue.RemoveAt(0);
	}
	Queue.Add(SentenceName);

	if (NextUpdateTime <= 0.0f)
	{
		// The queue was idle, so the suit clears its throat and starts shortly.
		NextUpdateTime = Now + FirstUpdateTime;
	}
}

void FLambdaSuitVoice::StartSentence(UObject* WorldContext, const FString& SentenceName, float Now)
{
	const FSourceSentence* Sentence = FSourceSentences::Get().Find(SentenceName);
	if (!Sentence)
	{
		UE_LOG(LogLambda, Verbose, TEXT("suit: no sentence '%s'"), *SentenceName);
		return;
	}
	SpeakingWords = Sentence->Words;
	WordIndex = 0;
	NextWordTime = Now;
}

void FLambdaSuitVoice::SpeakNextWord(UObject* WorldContext, float Now)
{
	const FSourceVoxWord& Word = SpeakingWords[WordIndex++];

	// A word names a file, not a soundscript entry - VOX_LoadWord appends the extension the same way. Without
	// it the sound layer would look the word up as a script name and find nothing.
	const FString WavePath = Word.Wave + TEXT(".wav");

	float ScriptVolume = 1.0f, ScriptPitch = 1.0f;
	ULambdaSoundWave* Wave = FLambdaSoundCache::Get().CreateWaveResolved(
		WorldContext, WavePath, /*bLoop=*/ false, ScriptVolume, ScriptPitch);

	// voxword_t percentages: 100 is the recording as it was made.
	const float Volume = FMath::Max(0.0f, Word.Volume / 100.0f);
	const float Pitch = FMath::Max(0.01f, Word.Pitch / 100.0f);

	// How long before the next word starts. The whole point of a sentence is that the words run together, so
	// this is the length of the wave at the rate it is being played, not a fixed gap.
	float Duration = 0.15f;
	if (const FSourceWavData* Data = FLambdaSoundCache::Get().GetWavData(WavePath))
	{
		const float Window = FMath::Clamp((Word.End - Word.Start) / 100.0f, 0.0f, 1.0f);
		Duration = Data->GetDuration() * Window / Pitch;
	}
	NextWordTime = Now + Duration;

	UE_LOG(LogLambda, Verbose, TEXT("suit word: %s (p%d v%d) %.2fs%s"),
		*WavePath, Word.Pitch, Word.Volume, Duration, Wave ? TEXT("") : TEXT(" MISSING"));

	if (Wave)
	{
		// The suit is inside the player's helmet: no attenuation, no direction, the same everywhere.
		UGameplayStatics::PlaySound2D(WorldContext, Wave, Volume, Pitch);
	}
}

void FLambdaSuitVoice::Tick(UObject* WorldContext, float Now)
{
	// Mid-sentence: keep the words coming.
	if (SpeakingWords.Num() > 0)
	{
		if (Now >= NextWordTime)
		{
			if (WordIndex < SpeakingWords.Num())
			{
				SpeakNextWord(WorldContext, Now);
			}
			else
			{
				// Line finished. The next one waits out SUITUPDATETIME, so the suit does not gabble.
				SpeakingWords.Reset();
				WordIndex = 0;
				NextUpdateTime = Queue.Num() > 0 ? Now + UpdateTime : 0.0f;
			}
		}
		return;
	}

	if (NextUpdateTime <= 0.0f || Now < NextUpdateTime || Queue.Num() == 0)
	{
		return;
	}
	const FString Next = Queue[0];
	Queue.RemoveAt(0);
	StartSentence(WorldContext, Next, Now);
	if (SpeakingWords.Num() == 0)
	{
		// Nothing to say after all (no such sentence): do not stall the queue on it.
		NextUpdateTime = Queue.Num() > 0 ? Now + FirstUpdateTime : 0.0f;
	}
}
