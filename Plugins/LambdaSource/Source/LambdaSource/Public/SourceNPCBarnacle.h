#pragma once

#include "CoreMinimal.h"
#include "SourceNPCBase.h"
#include "SourceNPCBarnacle.generated.h"

/** Where the barnacle is in its meal (CNPC_Barnacle's m_bLiftingPrey / m_bSwallowingPrey / m_flDigestFinish). */
enum class EBarnaclePhase : uint8
{
	Idle,			// tongue down, waiting for something to touch it
	Lifting,		// hauling the victim up towards the mouth
	Biting,			// victim at the mouth, being bitten
	Digesting,		// meal swallowed, bloated and burping
	Dead,
};

/**
 * npc_barnacle: the ceiling feeder (game/server/hl2/npc_barnacle.cpp). It hangs from its spawn point with its
 * tongue reaching down to just above the floor; whatever walks into the tongue is hauled up at
 * BARNACLE_PULL_SPEED with the pull throbbing on a sine, bitten at the mouth, and - if it was an NPC - swallowed
 * and digested. Killing the barnacle drops the victim. The tongue is the model's own tongue1..tongue8 bone
 * chain, stretched between root and tip the way C_NPC_Barnacle does on the client.
 *
 * The model is the HL:A barnacle converted by Tools/ImportSource2Model.py; the sounds are HL:A's, mapped onto
 * the NPC_Barnacle.* soundscript names this code emits.
 *
 * Not ported: the vphysics tongue spring and its shootable tongue tip, ragdoll victims (a bitten NPC is
 * swallowed whole instead), lifting physics props, the poison/bomb special cases, and the death gibs.
 */
UCLASS()
class LAMBDASOURCE_API ASourceNPCBarnacle : public ASourceNPCBase
{
	GENERATED_BODY()

public:
	ASourceNPCBarnacle(const FObjectInitializer& ObjectInitializer);

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void Spawn() override;
	virtual void NPCThink() override;
	virtual void Event_Killed(AActor* Attacker) override;
	virtual void IdleSound() override { EmitSound(TEXT("NPC_Barnacle.Idle")); }
	virtual void PainSound() override { EmitSound(TEXT("NPC_Barnacle.Pain")); }

private:
	/** Stretches tongue1..tongue8 between the root and the tip, and keeps the root/tip positions fresh. */
	void UpdateTongue();
	/** TongueTouchEnt: how far the tongue may hang here, and whoever is touching it. */
	float TongueTouchLength(AActor*& OutTouch) const;
	/** AttachTongueToTarget. */
	void AttachTongue(AActor* NewVictim);
	/** LiftPrey / PullEnemyTorwardsMouth, once per think. */
	void LiftPrey();
	/** BitePrey: the bite lands. */
	void BitePrey();
	/** LostPrey: let go of whatever is on the tongue. */
	void LostPrey();
	/** The victim hangs from the tongue: gravity off and our velocity, or their own feet back. */
	void SetVictimHeld(AActor* HeldVictim, bool bHeld);

	EBarnaclePhase Phase = EBarnaclePhase::Idle;
	TWeakObjectPtr<AActor> Victim;

	/** m_flAltitude: how far below the root the tongue tip hangs, Source units. */
	float AltitudeUnits = 0.0f;
	/** m_flRestUnitsAboveGround. */
	float RestUnitsAboveGround = 16.0f;
	/** m_flBarnaclePullSpeed (BARNACLE_PULL_SPEED). */
	float PullSpeedUnits = 80.0f;
	/** m_flLocalTimer, driving the sine of the pull. */
	float LocalTimer = 0.0f;
	/** m_flDigestFinish. */
	float DigestFinishTime = 0.0f;
	float NextBiteTime = 0.0f;
	/** PlayLiftingScream's one scream per meal. */
	bool bScreamed = false;
	/** The ceiling flip has been applied (first think, after the spawner placed us). */
	bool bMounted = false;
	/** The held victim's gravity, put back when it is released. */
	float VictimSavedGravityScale = 1.0f;

	/** Cached world positions of the tongue root (at the mouth) and tip. */
	FVector TongueRootCm = FVector::ZeroVector;
	FVector TongueTipCm = FVector::ZeroVector;
};
