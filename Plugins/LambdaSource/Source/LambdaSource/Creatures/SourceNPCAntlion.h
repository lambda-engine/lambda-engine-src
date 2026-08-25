#pragma once

#include "CoreMinimal.h"
#include "Creatures/SourceNPCBase.h"
#include "SourceNPCAntlion.generated.h"

/**
 * npc_antlion (game/server/hl2/npc_antlion.cpp), the parts a single hostile antlion needs.
 *
 * It runs the player down, swipes when it is in reach, and pounces from further out - the leap being the thing
 * that makes an antlion an antlion rather than a fast zombie. The swipes and the pounce are all driven by the
 * animation's own events, which is how Source does it: the attack sequence plays and AE_ANTLION_MELEE_HIT1,
 * HIT2 and MELEE_POUNCE do the hitting at the frame the animator marked.
 *
 * Damage and health come from cfg/skill.cfg (sk_antlion_health, sk_antlion_swipe_damage, sk_antlion_jump_damage);
 * the ranges, the view punches and the shove behind each hit are npc_antlion.cpp's own constants.
 *
 * Not ported, and it is most of that file: burrowing and the burrow schedules, bugbait, antlion workers and
 * their spit, squads and follow/fight goals, thumper flight, flipping onto its back, and the wing effects. Those
 * are 5000 lines of a 5200 line NPC and every one of them wants a piece of the AI that is not here yet.
 */
UCLASS()
class LAMBDASOURCE_API ASourceNPCAntlion : public ASourceNPCBase
{
	GENERATED_BODY()

public:
	ASourceNPCAntlion(const FObjectInitializer& ObjectInitializer);

protected:
	virtual void Spawn() override;
	virtual void NPCThink() override;
	virtual void HandleAnimEvent(int32 EventId, const FString& EventName, const FString& Options) override;
	virtual void Landed(const FHitResult& Hit) override;

	virtual void IdleSound() override { EmitSound(TEXT("NPC_Antlion.Idle")); }
	virtual void AlertSound() override { EmitSound(TEXT("NPC_Antlion.Distracted")); }
	virtual void PainSound() override { EmitSound(TEXT("NPC_Antlion.Pain")); }
	virtual void DeathSound() override { EmitSound(TEXT("NPC_Antlion.Pain")); }

	/** CNPC_Antlion::MeleeAttack: a hull swept forward, DMG_SLASH, with a view punch and a shove behind it. */
	void MeleeAttack(float DistUnits, float Damage, const FRotator& ViewPunch, const FVector& VelocityPunchUnits);

	/** CNPC_Antlion::StartJump: the pounce, solved as a ballistic arc onto the enemy. */
	void StartJump();
	/** Whether the enemy is in the band the pounce covers, and reachable in a straight line. */
	bool CanPounce(float DistUnits) const;

	void UpdateEnemy();
	void SetEnemy(AActor* NewEnemy);
	void ChaseEnemy();

	// npc_antlion.cpp
	static constexpr float ANTLION_MELEE1_RANGE = 100.0f;
	static constexpr float ANTLION_JUMP_MIN = 128.0f;
	static constexpr float ANTLION_JUMP_MAX = 1024.0f;
	static constexpr float ANTLION_JUMP_MAX_RISE = 512.0f;
	/** GetEnemies()->SetFreeKnowledgeDuration: how long it keeps an enemy it can no longer see. */
	static constexpr float ENEMY_FREE_KNOWLEDGE = 5.0f;

	TWeakObjectPtr<AActor> Enemy;
	FVector LastKnownEnemyPos = FVector::ZeroVector;
	float LastSeenEnemyTime = -1.0f;

	bool bAttacking = false;		// a melee or pounce sequence is playing
	bool bMidJump = false;			// m_bMidJump
	float NextAttackTime = 0.0f;	// m_flNextAttack
	float SwipeDamage = 5.0f;		// sk_antlion_swipe_damage
	float JumpDamage = 5.0f;		// sk_antlion_jump_damage
	float RunSpeedUnits = 0.0f;
	float WaitUntilTime = 0.0f;
	float LastDebugLogTime = -10.0f;
};
