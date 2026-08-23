#pragma once

#include "CoreMinimal.h"
#include "Engine/DamageEvents.h"

/** shareddefs.h damage types (the subset the ported code tests). */
namespace SourceDamageType
{
	constexpr int32 DMG_GENERIC = 0;
	constexpr int32 DMG_CRUSH = (1 << 0);
	constexpr int32 DMG_BULLET = (1 << 1);
	constexpr int32 DMG_SLASH = (1 << 2);
	constexpr int32 DMG_BURN = (1 << 3);
	constexpr int32 DMG_BLAST = (1 << 6);
	constexpr int32 DMG_CLUB = (1 << 7);
	constexpr int32 DMG_SHOCK = (1 << 8);
	constexpr int32 DMG_SONIC = (1 << 9);
	constexpr int32 DMG_BUCKSHOT = (1 << 13);
	constexpr int32 DMG_SNIPER = (1 << 25);
	constexpr int32 DMG_REMOVENORAGDOLL = (1 << 22);
}

/** shareddefs.h hit groups, as studiomdl writes them into hitboxes. */
namespace SourceHitGroup
{
	constexpr int32 HITGROUP_GENERIC = 0;
	constexpr int32 HITGROUP_HEAD = 1;
	constexpr int32 HITGROUP_CHEST = 2;
	constexpr int32 HITGROUP_STOMACH = 3;
	constexpr int32 HITGROUP_LEFTARM = 4;
	constexpr int32 HITGROUP_RIGHTARM = 5;
	constexpr int32 HITGROUP_LEFTLEG = 6;
	constexpr int32 HITGROUP_RIGHTLEG = 7;
	constexpr int32 HITGROUP_GEAR = 10;
}

/**
 * CTakeDamageInfo's physics and hit-location side on top of UE's point damage: the damage force (an impulse,
 * kg*cm/s here; Source keeps it in kg*in/s) and where it was applied, the damage type bits, and the hit group of
 * the hitbox the trace struck. Ragdolls are kicked with the force (CalcDamageForceVector -> BecomeRagdoll);
 * NPCs scale damage and flinch by the hit group.
 */
struct LAMBDASOURCE_API FSourceDamageEvent : public FPointDamageEvent
{
	/** m_vecDamageForce, in kg*cm/s. Zero means "none given" and CalcDamageForceVector makes one up from the attacker. */
	FVector DamageForce = FVector::ZeroVector;
	/** m_vecDamagePosition, world. */
	FVector DamagePosition = FVector::ZeroVector;
	/** m_bitsDamageType (SourceDamageType::DMG_*). */
	int32 DamageType = SourceDamageType::DMG_GENERIC;
	/** trace_t::hitgroup of the hitbox hit (SourceHitGroup::HITGROUP_*). */
	int32 HitGroup = SourceHitGroup::HITGROUP_GENERIC;

	FSourceDamageEvent() {}
	FSourceDamageEvent(float InDamage, const FHitResult& InHitInfo, const FVector& InShotDirection, TSubclassOf<UDamageType> InDamageTypeClass,
		const FVector& InDamageForce, int32 InDamageType = SourceDamageType::DMG_GENERIC, int32 InHitGroup = SourceHitGroup::HITGROUP_GENERIC)
		: FPointDamageEvent(InDamage, InHitInfo, InShotDirection, InDamageTypeClass)
		, DamageForce(InDamageForce)
		, DamagePosition(InHitInfo.ImpactPoint)
		, DamageType(InDamageType)
		, HitGroup(InHitGroup)
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
