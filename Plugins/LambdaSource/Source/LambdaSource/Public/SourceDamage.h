#pragma once

#include "CoreMinimal.h"
#include "Engine/DamageEvents.h"

/**
 * CTakeDamageInfo's physics side on top of UE's point damage: the damage force (an impulse, kg*cm/s here; Source
 * keeps it in kg*in/s) and where it was applied. Ragdolls are kicked with it (CalcDamageForceVector -> BecomeRagdoll).
 */
struct LAMBDASOURCE_API FSourceDamageEvent : public FPointDamageEvent
{
	/** m_vecDamageForce, in kg*cm/s. Zero means "none given" and CalcDamageForceVector makes one up from the attacker. */
	FVector DamageForce = FVector::ZeroVector;
	/** m_vecDamagePosition, world. */
	FVector DamagePosition = FVector::ZeroVector;

	FSourceDamageEvent() {}
	FSourceDamageEvent(float InDamage, const FHitResult& InHitInfo, const FVector& InShotDirection, TSubclassOf<UDamageType> InDamageTypeClass,
		const FVector& InDamageForce)
		: FPointDamageEvent(InDamage, InHitInfo, InShotDirection, InDamageTypeClass)
		, DamageForce(InDamageForce)
		, DamagePosition(InHitInfo.ImpactPoint)
	{}

	static const int32 ClassID = 0x4C534443;	// 'LSDC'
	virtual int32 GetTypeID() const override { return FSourceDamageEvent::ClassID; }
	virtual bool IsOfType(int32 InID) const override { return FSourceDamageEvent::ClassID == InID || FPointDamageEvent::IsOfType(InID); }
};

namespace SourceDamage
{
	/**
	 * hl2_gamerules.cpp BULLET_IMPULSE(grains, ftpersec): the impulse an ammo type's bullet carries, in kg*in/s,
	 * with Valve's 3.5x exaggeration. ((ftpersec)*12*BULLET_MASS_GRAINS_TO_KG(grains)*BULLET_IMPULSE_EXAGGERATION)
	 */
	FORCEINLINE float BulletImpulse(float Grains, float FeetPerSec)
	{
		const float BulletMassGrainsToKg = 0.0000648f;	// BULLET_MASS_GRAINS_TO_KG(grains) = grains * 0.0000648
		return FeetPerSec * 12.0f * (Grains * BulletMassGrainsToKg) * 3.5f;
	}
}
