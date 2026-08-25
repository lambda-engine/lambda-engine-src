#pragma once

#include "CoreMinimal.h"
#include "Creatures/SourceNPCBase.h"
#include "SourceNPCZombie.generated.h"

/** CNPC_BaseZombie::ShouldReleaseHeadcrab's answer (HeadcrabRelease_t). */
enum class ESourceHeadcrabRelease : uint8
{
	No,
	Immediate,			// a live headcrab hops off the corpse
	Ragdoll,			// the headcrab comes off dead
};

/**
 * npc_zombie: the HL2 classic zombie (game/server/hl2/npc_BaseZombie.cpp + npc_zombie.cpp), the parts a single
 * hostile zombie needs. Walks at the player, claws when in reach (the animation's attack events do the hitting,
 * CheckTraceHullAttack-style), moans and screams from its soundscripts, flinches by hit group, takes double
 * damage to the head and half from body shots (ZOMBIE_BULLET_DAMAGE_SCALE), and on death releases its headcrab:
 * alive off a body kill, dead off a hard head kill (ShouldReleaseHeadcrab), the corpse then ragdolling headless.
 *
 * Not ported: prop swatting, the torso/legs split (DieChopped/BecomeTorso), slumped spawns, burning, squads.
 */
UCLASS()
class LAMBDASOURCE_API ASourceNPCZombie : public ASourceNPCBase
{
	GENERATED_BODY()

public:
	ASourceNPCZombie(const FObjectInitializer& ObjectInitializer);

	/** The hit reaction HL2 gives the headcrab on the zombie's head: the HC_Body_Bone hitbox is HITGROUP_HEAD. */
	virtual float GetHitgroupDamageMultiplier(int32 HitGroup, const FSourceDamageEvent& Info) const override;

protected:
	virtual void Spawn() override;
	virtual void NPCThink() override;
	virtual void HandleAnimEvent(int32 EventId, const FString& EventName, const FString& Options) override;
	virtual void TraceAttack(const FSourceDamageEvent& Info) override;
	virtual void OnTakeDamage_Alive(float Damage, AActor* Attacker, const FSourceDamageEvent& Info) override;
	virtual bool IsHeavyDamage(float Damage, const FSourceDamageEvent& Info) const override;

	virtual void OnMovementBlocked() override;
	virtual void IdleSound() override;
	virtual void AlertSound() override;
	virtual void PainSound() override { EmitSound(TEXT("Zombie.Pain")); }
	virtual void DeathSound() override { EmitSound(TEXT("Zombie.Die")); }
	void AttackSound() { EmitSound(TEXT("Zombie.Attack")); }
	void AttackHitSound() { EmitSound(TEXT("Zombie.AttackHit")); }
	void AttackMissSound() { EmitSound(TEXT("Zombie.AttackMiss")); }
	void FootstepSound(bool bRightFoot) { EmitSound(bRightFoot ? TEXT("Zombie.FootstepRight") : TEXT("Zombie.FootstepLeft")); }
	void FootscuffSound(bool bRightFoot) { EmitSound(bRightFoot ? TEXT("Zombie.ScuffRight") : TEXT("Zombie.ScuffLeft")); }

	/** CNPC_BaseZombie::ClawAttack: a hull swept forward by the claw's reach; whatever it hits takes DMG_SLASH. */
	void ClawAttack(float DistUnits, float Damage, const FRotator& ViewPunch, const FVector& VelocityPunchUnits);

	ESourceHeadcrabRelease ShouldReleaseHeadcrab(const FSourceDamageEvent& Info, float DamageThreshold) const;
	/** CNPC_BaseZombie::ReleaseHeadcrab: spawns the headcrab at the "headcrab" attachment, live or as a dead ragdoll. */
	void ReleaseHeadcrab(const FVector& Origin, const FVector& VelocityCm, bool bRemoveHead, bool bRagdollCrab, const FVector& RagdollImpulse);
	/** CNPC_BaseZombie::RemoveHead -> SetZombieModel: the headcrab bodygroup goes away. */
	void RemoveHead();

	/** CZombie / CNPC_BaseZombie::MaxYawSpeed by activity. */
	float ZombieMaxYawSpeed() const;

private:
	bool bHeadShot = false;		// m_bHeadShot: the last TraceAttack hit HITGROUP_HEAD
	bool bHeadless = false;		// m_fIsHeadless
	float NextMoanTime = 0.0f;	// m_flNextMoanSound
	float NextPoundTime = 0.0f;	// when the zombie may pound at an obstruction again
	int32 HeadcrabBodyPart = INDEX_NONE;	// ZOMBIE_BODYGROUP_HEADCRAB
};
