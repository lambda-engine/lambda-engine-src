#pragma once

#include "CoreMinimal.h"
#include "Entities/SourcePropPhysics.h"
#include "SourceGrenade.generated.h"

class ULambdaMaterialLibrary;

/**
 * A thrown fragmentation grenade (CGrenadeFrag / basegrenade_timed).
 *
 * Thrown by the player and by NPCs alike - the only difference is who owns it and which skill value its
 * damage comes from. It is a physics object like any prop (VPhysicsInitNormal on its own collision model), so
 * it tumbles through the air, bounces and rolls to a stop the way a thrown thing does, and after its fuse runs
 * out does radius damage that falls off with distance and stops at anything solid, which is what makes cover
 * worth taking. A bullet sets it off early: Source gives it one point of health.
 *
 * It is deliberately visible and audible for its whole life: it blips once a second, and three times a second
 * in its last second and a half (FRAG_GRENADE_WARN_TIME), and F.E.A.R.'s soldiers shout and dive. An NPC finds
 * one through FindLiveGrenadeNear, so a grenade is a thing in the world that anything can notice rather than a
 * message sent to a list of victims.
 */
UCLASS()
class LAMBDASOURCE_API ASourceGrenade : public ASourcePropPhysics
{
	GENERATED_BODY()

public:
	ASourceGrenade(const FObjectInitializer& ObjectInitializer);

	/**
	 * Arms and throws one. Velocity is in UE cm/s; Damage and RadiusUnits come from skill.cfg, so the player's
	 * grenade and a soldier's differ exactly as Half-Life 2 has them. AngularVelocityDegrees is the tumble it
	 * leaves the hand with (CWeaponFrag::ThrowGrenade's AngularImpulse); zero picks Source's own random one.
	 */
	static ASourceGrenade* Throw(UWorld* World, AActor* Thrower, const FVector& Location, const FVector& Velocity,
		float FuseSeconds, float Damage, float RadiusUnits, ULambdaMaterialLibrary* Materials,
		const FVector& AngularVelocityDegrees = FVector::ZeroVector);

	/** The nearest live grenade within the radius, ignoring ones this actor threw itself. */
	static ASourceGrenade* FindLiveGrenadeNear(const UWorld* World, const FVector& Position, float RadiusCm,
		const AActor* Ignoring);

	/** Where it is and how long is left - what an AI needs to decide whether to run. */
	float GetTimeToDetonation() const;
	const AActor* GetThrower() const { return Thrower.Get(); }

	virtual void Tick(float DeltaSeconds) override;
	/** CBaseGrenade::Event_Killed: a grenade that is shot goes off where it lies. */
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

protected:
	void Blip();
	void Detonate();

	/** The map's materials, for the explosion's sprites. */
	TWeakObjectPtr<ULambdaMaterialLibrary> Materials;
	TWeakObjectPtr<AActor> Thrower;
	float DetonateTime = 0.0f;
	/** m_flWarnAITime: from here on the blips come fast. */
	float WarnTime = 0.0f;
	float NextBlipTime = 0.0f;
	float Damage = 125.0f;
	float RadiusCm = 0.0f;
	bool bDetonated = false;
	/** The last place it was known to be clear of the world, for the sweep that keeps it from passing through. */
	FVector SafePosition = FVector::ZeroVector;
	bool bHaveSafePosition = false;
};
