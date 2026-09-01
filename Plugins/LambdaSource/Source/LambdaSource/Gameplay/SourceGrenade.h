#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SourceGrenade.generated.h"

class ULambdaMaterialLibrary;
class USourceStudioModelComponent;
class UProjectileMovementComponent;

/**
 * A thrown fragmentation grenade (CGrenadeFrag / basegrenade_timed).
 *
 * Thrown by the player and by NPCs alike - the only difference is who owns it and which skill value its
 * damage comes from. It flies, bounces off the world, and after its fuse runs out does radius damage that
 * falls off with distance and stops at anything solid, which is what makes cover worth taking.
 *
 * It is deliberately visible and audible for its whole life: Source ticks a warning beep, and F.E.A.R.'s
 * soldiers shout and dive. An NPC finds one through FindLiveGrenadeNear, so a grenade is a thing in the
 * world that anything can notice rather than a message sent to a list of victims.
 */
UCLASS()
class LAMBDASOURCE_API ASourceGrenade : public AActor
{
	GENERATED_BODY()

public:
	ASourceGrenade();

	/**
	 * Arms and throws one. Velocity is in UE cm/s; Damage and RadiusUnits come from skill.cfg, so the
	 * player's grenade and a soldier's differ exactly as Half-Life 2 has them.
	 */
	static ASourceGrenade* Throw(UWorld* World, AActor* Thrower, const FVector& Location, const FVector& Velocity,
		float FuseSeconds, float Damage, float RadiusUnits, ULambdaMaterialLibrary* Materials);

	/** The nearest live grenade within the radius, ignoring ones this actor threw itself. */
	static ASourceGrenade* FindLiveGrenadeNear(const UWorld* World, const FVector& Position, float RadiusCm,
		const AActor* Ignoring);

	/** Where it is and how long is left - what an AI needs to decide whether to run. */
	float GetTimeToDetonation() const;
	const AActor* GetThrower() const { return Thrower.Get(); }

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

	UFUNCTION()
	void OnBounce(const FHitResult& Hit, const FVector& Velocity);

	void Detonate();

	UPROPERTY(Transient)
	TObjectPtr<USourceStudioModelComponent> Model;

	UPROPERTY(Transient)
	TObjectPtr<UProjectileMovementComponent> Movement;

	TWeakObjectPtr<AActor> Thrower;
	float DetonateTime = 0.0f;
	float Damage = 125.0f;
	float RadiusCm = 0.0f;
	bool bDetonated = false;
};
