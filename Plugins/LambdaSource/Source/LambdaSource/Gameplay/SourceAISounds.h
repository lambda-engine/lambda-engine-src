#pragma once

#include "CoreMinimal.h"

/** CSoundEnt's sound types, cut to the ones anything here emits. */
enum class ESourceAISoundType : uint8
{
	Combat,		// a shot, a ricochet, a blow landing - something violent happened here
	Player,		// footsteps and the other noises a body makes moving about
	Danger,		// get away from this spot
};

/**
 * CSoundEnt (game/server/soundent.cpp): the world's short-term memory of noises, so an AI can hear.
 *
 * Source keeps sounds in a list rather than sending them to listeners, and that indirection is the whole
 * point: a noise is a thing that happened at a place, and any AI near enough may or may not notice it. An
 * NPC that reacts to a gunshot it could not possibly have heard is the classic tell of a cheating AI.
 *
 * Each sound carries where it was, how far it carries, and when it stops mattering. Nothing owns them.
 */
class LAMBDASOURCE_API FSourceAISounds
{
public:
	static FSourceAISounds& Get();

	/**
	 * CSoundEnt::InsertSound. RadiusUnits is Source's "volume" - how far the noise reaches, not how loud it
	 * is - and DurationSeconds is how long it stays worth reacting to.
	 */
	void Insert(ESourceAISoundType Type, const FVector& Position, float RadiusUnits, float DurationSeconds,
		const AActor* Owner, const UWorld* World);

	/**
	 * The most interesting live sound this listener could hear: within its own radius of the listener, not
	 * made by the listener, and not expired. Combat beats footsteps when both are audible, because a
	 * gunshot is more worth turning round for than a floorboard.
	 */
	bool Loudest(const FVector& ListenerPos, const AActor* Listener, const UWorld* World,
		FVector& OutPosition, ESourceAISoundType& OutType) const;

	void Reset() { Sounds.Reset(); }

private:
	struct FEntry
	{
		FVector Position = FVector::ZeroVector;
		float RadiusCm = 0.0f;
		float ExpireTime = 0.0f;
		ESourceAISoundType Type = ESourceAISoundType::Combat;
		TWeakObjectPtr<const AActor> Owner;
	};

	/** Small and overwritten oldest-first: a firefight makes a lot of noise and none of it matters for long. */
	static constexpr int32 MaxSounds = 32;
	mutable TArray<FEntry> Sounds;
};
