#pragma once

#include "CoreMinimal.h"

#include "Audio/SourceSentences.h"

/**
 * The HEV suit's voice - CBasePlayer's suit update queue, from player.cpp.
 *
 * The suit does not speak the instant something happens. Lines are queued, played one at a time with a gap
 * between them, and each may declare how long it must be before it is allowed to say that particular thing
 * again - which is what keeps "minor fracture" from firing on every bullet of a burst, and what makes the suit
 * sound like it is triaging rather than narrating.
 *
 * Each line is a sentence rather than a recording: see FSourceSentences. Playing one means playing its words
 * back to back, so the voice keeps a word cursor and starts the next word as the last one runs out.
 */
class FLambdaSuitVoice
{
public:
	/** CSUITPLAYLIST: how many lines may be waiting. Beyond this the oldest is dropped, as Source does. */
	static constexpr int32 MaxQueued = 4;
	/** SUITUPDATETIME: the gap between one line finishing and the next starting. */
	static constexpr float UpdateTime = 3.5f;
	/** SUITFIRSTUPDATETIME: how long after the first line is queued before the suit starts talking. */
	static constexpr float FirstUpdateTime = 0.1f;

	// player.h's SUIT_NEXT_IN_*, in seconds.
	static constexpr float RepeatOK = 0.0f;
	static constexpr float NextIn30Sec = 30.0f;
	static constexpr float NextIn1Min = 60.0f;
	static constexpr float NextIn5Min = 300.0f;
	static constexpr float NextIn10Min = 600.0f;
	static constexpr float NextIn30Min = 1800.0f;

	/**
	 * SetSuitUpdate: queue a sentence, unless it is still inside its no-repeat window.
	 *
	 * NoRepeatSeconds is how long to refuse this same line for afterwards; zero allows it again at once.
	 */
	void SetSuitUpdate(UObject* WorldContext, const FString& SentenceName, float NoRepeatSeconds);

	/** CheckSuitUpdate: called every frame. Starts the next line, and the next word of the line being spoken. */
	void Tick(UObject* WorldContext, float Now);

	/** Drops the queue and stops talking - a new map, or death. */
	void Reset();

	/** Whether the suit is mid-sentence, which the HUD can use to light its own indicator. */
	bool IsSpeaking() const { return SpeakingWords.Num() > 0; }

private:
	void StartSentence(UObject* WorldContext, const FString& SentenceName, float Now);
	void SpeakNextWord(UObject* WorldContext, float Now);

	/** Sentence names waiting to be spoken, oldest first. */
	TArray<FString> Queue;
	/** When the next queued line may start. Zero means the queue is idle. */
	float NextUpdateTime = 0.0f;

	/** The words of the line being spoken, and where we are in them. */
	TArray<FSourceVoxWord> SpeakingWords;
	int32 WordIndex = 0;
	float NextWordTime = 0.0f;

	/** m_rgiSuitNoRepeat / m_rgflSuitNoRepeatTime: a line, and the time it may be said again. */
	TMap<FString, float> NoRepeatUntil;
};
