#pragma once

#include "CoreMinimal.h"

/** Source fires an output "always" unless the map limits it (entityoutput.h EVENT_FIRE_ALWAYS). */
#define SOURCE_EVENT_FIRE_ALWAYS (-1)

/**
 * One connection made in Hammer: "<target>,<input>,<parameter>,<delay>,<timesToFire>".
 * A port of CEventAction (game/server/cbase.cpp). The delimiter is ESC (0x1B) when the map uses it - vbsp switches
 * to that so parameters may contain commas - otherwise a comma.
 */
struct LAMBDASOURCE_API FSourceEventAction
{
	FString Target;
	FString TargetInput = TEXT("Use");	// CEventAction defaults to "Use" when the field is empty
	FString Parameter;
	float Delay = 0.0f;
	int32 TimesToFire = SOURCE_EVENT_FIRE_ALWAYS;

	/** Remaining fires; counts down when TimesToFire is limited. */
	int32 FiresLeft = SOURCE_EVENT_FIRE_ALWAYS;

	static FSourceEventAction Parse(const FString& ActionData);
};

/** All connections made from one named output, e.g. "OnPressed". */
struct LAMBDASOURCE_API FSourceOutput
{
	FString Name;
	TArray<FSourceEventAction> Actions;
};

/** An input queued to fire at a future time (CEventQueue). */
struct LAMBDASOURCE_API FSourceQueuedEvent
{
	FString Target;
	FString Input;
	FString Parameter;
	TWeakObjectPtr<AActor> Activator;
	TWeakObjectPtr<AActor> Caller;
	float FireTime = 0.0f;
};
