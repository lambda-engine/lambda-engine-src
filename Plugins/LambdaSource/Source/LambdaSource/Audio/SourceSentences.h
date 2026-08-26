#pragma once

#include "CoreMinimal.h"

/**
 * One word of a sentence, with the parameter block that was in force when it was named.
 *
 * vox_private.h's voxword_t. Percentages throughout: volume and pitch are "increase percent" where 100 is the
 * recording as it was made, and start/end trim the wave to that window of itself.
 */
struct LAMBDASOURCE_API FSourceVoxWord
{
	FString Wave;				// relative to sound/, e.g. "hev_vox/minor_fracture"
	int32 Volume = 100;
	int32 Pitch = 100;			// vox writes -1 for "unset"; that is normalised to 100 on parse
	int32 Start = 0;			// percent of the wave to skip at the front
	int32 End = 100;			// percent of the wave to stop at
	int32 TimeCompress = 0;		// percent of the wave to skip during playback, without shifting pitch
};

/**
 * A named sentence: the words, in order.
 *
 * Half-Life speaks in sentences rather than lines. "HEV_DMG4" is not a recording of "minor fracture detected" -
 * it is a recipe, three boops at pitch 160 and then the word "minor_fracture", and the engine plays those four
 * waves back to back. It is why the suit can say a hundred different things out of fifty-five recordings, and
 * why it can read a number out loud.
 */
struct LAMBDASOURCE_API FSourceSentence
{
	FString Name;
	TArray<FSourceVoxWord> Words;

	/** How long this will take to say, given a way of measuring each wave. Used to space the queue. */
	float EstimateDuration(TFunctionRef<float(const FString&)> WaveDuration) const;
};

/**
 * scripts/sentences.txt, the way Source's vox.cpp reads it.
 *
 * A line is a name and then the words:
 *
 *     HEV_DMG4 hev_vox/(p160) boop, boop, boop, (p100) minor_fracture
 *
 * The first word may carry a directory, which then applies to every word after it in that sentence. A bracketed
 * block sets parameters; attached to a word it applies to that word alone, standing on its own it becomes the
 * default for the words that follow (VOX_ParseWordParams: "if the string has zero length, this was an isolated
 * parameter block"). Commas are word separators and nothing more - the pauses in the suit's speech are silence
 * at the ends of the recordings, not something the parser inserts.
 *
 * Black Mesa's file adds a trailing {Len ... closecaption ...} block that Valve's does not have; it is skipped.
 */
class LAMBDASOURCE_API FSourceSentences
{
public:
	static FSourceSentences& Get();

	/** Reads scripts/sentences.txt. Safe to call repeatedly; only the first call does work. */
	void Initialize();
	/** Drops everything so the next Initialize() re-reads (a new map or game directory). */
	void Reset();

	/** Looks up by name, with or without the leading '!' the game code writes. Null if there is no such sentence. */
	const FSourceSentence* Find(const FString& Name);

	int32 Num() const { return Sentences.Num(); }

private:
	FSourceSentences() = default;
	void ParseLine(const FString& Line);

	TMap<FString, FSourceSentence> Sentences;	// keyed by uppercase name
	bool bInitialized = false;
};
