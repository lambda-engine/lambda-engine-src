#pragma once

#include "CoreMinimal.h"
#include "Creatures/SourceNPCBase.h"
#include "SourceNPCHeadcrab.generated.h"

/**
 * npc_headcrab - a port of CBaseHeadcrab / CHeadcrab (game/server/hl2/npc_headcrab.cpp).
 *
 * What is ported: the hull, view offset, field of view, health and bite damage (sk_headcrab_* from skill.cfg),
 * the sounds, RangeAttack1Conditions (jump range 48..256 units, facing, clear line), the jump-attack sequence
 * with its AE_HEADCRAB_JUMP_TELEGRAPH / AE_HEADCRAB_JUMPATTACK events, JumpAttack's ballistic velocity, Leap and
 * LeapTouch's bite, the failed-attack delay, the wake-angry alert, and the combat-state coo.
 *
 * What is not: burrowing, ceiling hanging, canisters, drowning, squads, and pathfinding - the chase is a straight
 * line at the run animation's ground speed, since the node graph is not loaded.
 */
UCLASS()
class LAMBDASOURCE_API ASourceNPCHeadcrab : public ASourceNPCBase
{
	GENERATED_BODY()

public:
	ASourceNPCHeadcrab(const FObjectInitializer& ObjectInitializer);

	virtual void Landed(const FHitResult& Hit) override;

protected:
	virtual void Spawn() override;
	virtual void NPCThink() override;
	virtual void HandleAnimEvent(int32 EventId, const FString& EventName, const FString& Options) override;
	virtual void OnTakeDamage_Alive(float Damage, AActor* Attacker, const FSourceDamageEvent& Info) override;

	virtual void IdleSound() override { EmitSound(TEXT("NPC_HeadCrab.Idle")); }
	virtual void AlertSound() override { EmitSound(TEXT("NPC_HeadCrab.Alert")); }
	virtual void PainSound() override { EmitSound(TEXT("NPC_HeadCrab.Pain")); }
	virtual void DeathSound() override { EmitSound(TEXT("NPC_HeadCrab.Die")); }
	void TelegraphSound() { EmitSound(TEXT("NPC_HeadCrab.Alert")); }
	void AttackSound() { EmitSound(TEXT("NPC_Headcrab.Attack")); }
	void BiteSound() { EmitSound(TEXT("NPC_HeadCrab.Bite")); }

	/** CBaseHeadcrab::RangeAttack1Conditions, reduced to the answer the schedule needs. */
	enum class EAttackCondition : uint8 { CanAttack, NotFacing, TooClose, TooFar, Blocked, NotReady };
	EAttackCondition RangeAttack1Conditions(float FlDot, float FlDistUnits) const;
	/**
	 * Whether there is room to launch: the crab's own hull swept the first part of the way to its enemy.
	 *
	 * CBaseHeadcrab::RangeAttack1Conditions does this with an eight unit hull trace, and only when the enemy is
	 * higher up than the crab. Eight units is enough to catch a crab jammed against something, which is what it
	 * was for. It is not enough for a crab that has just come round a corner: its eyes clear the corner before
	 * its body does, so it leaps, clips the corner and lands nowhere. Ours sweeps a hull's width, and does it
	 * whichever way the enemy is, so the crab keeps walking until it has actually rounded the corner.
	 */
	bool HasRoomToLeap() const;

	/** CBaseHeadcrab::JumpAttack + Leap: launch at a world position (the enemy's eyes) or hop randomly. */
	void JumpAttack(bool bRandomJump, const FVector& WorldPos);
	void Leap(const FVector3f& VelocityUnits);

	UFUNCTION()
	void OnCapsuleHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit);

	/** CBaseHeadcrab::LeapTouch. */
	void LeapTouch(AActor* Other);
	/** CBaseHeadcrab::TouchDamage: sk_headcrab_melee_dmg as a slash. */
	void TouchDamage(AActor* Other);

	void SetEnemy(AActor* NewEnemy);
	void UpdateEnemy();
	void ChaseEnemy();
	void BackAwayFromEnemy();

	// npc_headcrab.cpp
	static constexpr float HEADCRAB_MIN_JUMP_DIST = 48.0f;
	static constexpr float HEADCRAB_MAX_JUMP_DIST = 256.0f;

	/** How far ahead the crab must be able to move its own body before it will leap. */
	static constexpr float HEADCRAB_LEAP_CLEARANCE = 24.0f;
	static constexpr float HEADCRAB_IGNORE_WORLD_COLLISION_TIME = 0.5f;
	/** GetEnemies()->SetFreeKnowledgeDuration(5.0): how long it keeps an enemy it cannot see. */
	static constexpr float ENEMY_FREE_KNOWLEDGE = 5.0f;

	TWeakObjectPtr<AActor> Enemy;
	FVector LastKnownEnemyPos = FVector::ZeroVector;
	float LastSeenEnemyTime = -1.0f;

	bool bMidJump = false;				// m_bMidJump
	bool bCommittedToJump = false;		// m_bCommittedToJump
	FVector CommittedJumpPos = FVector::ZeroVector;	// m_vecCommittedJumpPos
	bool bAttackFailed = false;			// m_bAttackFailed
	bool bAttacking = false;			// the ACT_RANGE_ATTACK1 task is running
	float NextAttackTime = 0.0f;		// m_flNextAttack
	float IgnoreWorldCollisionTime = 0.0f;	// m_flIgnoreWorldCollisionTime
	float MeleeDamage = 5.0f;			// sk_headcrab_melee_dmg
	float RunSpeedUnits = 0.0f;			// from the ACT_RUN sequence's root motion
	float WaitUntilTime = 0.0f;			// TASK_WAIT_RANDOM after an attack
	float LastDebugLogTime = -10.0f;
};
