#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Formats/SourceBSPFile.h"
#include "World/SourceEntityIO.h"
#include "SourceEntity.generated.h"

class ASourceBSPWorldActor;

/**
 * Base for every entity spawned from the BSP entity lump: holds its keyvalues, targetname and spawnflags, and
 * implements the input/output plumbing (CBaseEntity::AcceptInput and COutputEvent::FireOutput).
 */
UCLASS(Abstract)
class LAMBDASOURCE_API ASourceEntity : public AActor
{
	GENERATED_BODY()

public:
	/** Reads the common keyvalues and parses every output connection declared on this entity. */
	virtual void InitializeEntity(const FSourceEntity& InEntity, ASourceBSPWorldActor* InWorldActor);

	/** Called once every entity in the map has been spawned, so targets can be resolved (CBaseEntity::Activate). */
	virtual void ActivateEntity() {}

	/**
	 * CBaseEntity::AcceptInput - runs a named input on this entity. Return true if it was handled.
	 * Base implementation handles nothing; derived entities override and call Super for unknown inputs.
	 */
	virtual bool AcceptInput(const FString& InputName, AActor* Activator, AActor* Caller, const FString& Parameter);

	/** COutputEvent::FireOutput - queues every connection made from this named output. */
	void FireOutput(const FString& OutputName, AActor* Activator, float ExtraDelay = 0.0f);

	/**
	 * COutputEvent::GetMaxDelay - the longest delay on this output, or -1 if the map connected nothing to it.
	 *
	 * Connections that have used up their fire count still count towards it, as they do in Source: the delay
	 * is a property of the wiring, not of what is left to send down it.
	 */
	float GetOutputMaxDelay(const FString& OutputName) const;

	/** CEventQueue::CancelEvents - drops the events this entity fired that have not gone out yet. */
	void CancelPendingOutputs();

	const FSourceEntity& GetEntity() const { return Entity; }
	const FString& GetTargetName() const { return TargetName; }
	int32 GetSpawnFlags() const { return SpawnFlags; }
	bool HasSpawnFlags(int32 Flags) const { return (SpawnFlags & Flags) != 0; }

	/** True if this entity's targetname matches, supporting Source's trailing '*' wildcard. */
	bool MatchesTargetName(const FString& Pattern) const;

protected:
	FSourceEntity Entity;
	FString TargetName;
	int32 SpawnFlags = 0;

	/** Output connections declared in the map, keyed by output name ("OnPressed"). */
	TArray<FSourceOutput> Outputs;

	TWeakObjectPtr<ASourceBSPWorldActor> WorldActor;
};
