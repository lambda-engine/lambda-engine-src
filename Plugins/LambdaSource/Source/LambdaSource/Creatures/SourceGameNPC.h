#pragma once

#include "CoreMinimal.h"
#include "Creatures/SourceNPCBase.h"
#include "Game/LambdaGameAPI.h"
#include "SourceGameNPC.generated.h"

class UAudioComponent;
class USourceStudioModelComponent;

/**
 * An NPC whose mind lives in LambdaGame.dll - the counterpart to ASourceGameEntity for brush entities and
 * ASourceGamePointEntity for point ones.
 *
 * The split is the project's usual one. Everything that is a body belongs here: the studio model and its
 * activities, the capsule, navigation, line-of-sight traces, taking damage, dying into a ragdoll, and the
 * voice. Everything that is a decision - goals, plans, targets, fear - lives across the boundary and speaks
 * through the NPC vocabulary in the API. ASourceNPCBase already is the body for the native NPCs; this class
 * only forwards its senses outward and its orders inward.
 *
 * The body knows what each npc classname looks like (model, hull, health) the same way the brush host knows
 * its geometry: appearance is the engine's, behaviour is the game's.
 */
UCLASS()
class LAMBDASOURCE_API ASourceGameNPC : public ASourceNPCBase
{
	GENERATED_BODY()

public:
	ASourceGameNPC(const FObjectInitializer& ObjectInitializer);

	virtual void InitializeFromEntity(const FSourceEntity& InEntity, ASourceBSPWorldActor* InWorldActor,
		ULambdaMaterialLibrary* Materials) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	/** Whether this class's look is known to the body. The spawn path asks before spawning a host. */
	static bool KnowsAppearanceOf(const FString& ClassName);

	// ---- the mind's orders, called from FLambdaGameDll ----

	/** Starts a navmesh move toward a UE-space goal. False when there is no route. */
	bool MindMoveTo(const FVector& Goal);
	bool MindMoveDone() const { return !bMoveActive; }
	void MindStopMoving();
	/** One trigger pull at a world position: pellets, spread, damage, tracer-ish impact, muzzle sound. */
	void MindShootAt(const FVector& TargetPoint, AActor* TargetActor, const lambda::NPCShotParams& Params);
	/** Speaks a soundscript on the voice, cutting whatever was playing. */
	bool MindSpeak(const FString& Soundscript);
	bool MindIsSpeaking() const;
	/** Finds a reachable point whose chest-height line to the threat the world blocks. Positions in UE space. */
	bool MindFindCover(const FVector& ThreatPos, float MinDistCm, float MaxDistCm, FVector& OutPos);
	/** Finds a reachable point that can see the target from a different side than we are on now. */
	bool MindFindFlank(const FVector& ThreatPos, float MinDistCm, float MaxDistCm, FVector& OutPos);
	/**
	 * CAI_BaseNPC::PointInSpread's job: would a shot at this target pass through one of our own first?
	 * Traced rather than measured off the line, because the trace is what the bullet will actually do.
	 */
	bool HasClearShotAt(const AActor* Target) const;

	/** Throws a live grenade from the hand, with the given velocity in UE cm/s. */
	void ThrowGrenade(const FVector& Velocity, float FuseSeconds);
	/**
	 * The velocity that lobs a grenade from the hand onto a spot in FuseSeconds under gravity, so it lands
	 * as the fuse runs out. False if the throw would be absurd - straight up, or faster than an arm.
	 */
	bool SolveThrowArc(const FVector& Target, float FuseSeconds, FVector& OutVelocity) const;

	/** The chest-height trace alone: is this point still cover from there? */
	bool IsPointCoverFrom(const FVector& Pos, const FVector& ThreatPos) const;

	using ASourceNPCBase::SetActivity;
	using ASourceNPCBase::IsActivityFinished;
	using ASourceNPCBase::EyePosition;
	using ASourceNPCBase::FVisible;
	using ASourceNPCBase::FInViewCone;
	using ASourceNPCBase::SetIdealYawToTarget;
	float GetHealthValue() const { return Health; }

protected:
	virtual void Spawn() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void NPCThink() override;
	virtual void OnTakeDamage_Alive(float Damage, AActor* Attacker, const FSourceDamageEvent& Info) override;
	virtual void Event_Killed(AActor* Attacker) override;
	virtual void OnMovementBlocked() override;

private:
	/** True while a MindMoveTo is being walked; NPCThink steers it and clears it on arrival or blockage. */
	bool bMoveActive = false;
	FVector MoveGoal = FVector::ZeroVector;

	lambda::IEntity* Behaviour = nullptr;
	lambda::EntityId GameId = lambda::InvalidEntity;

	/**
	 * The weapon in its hands: the world model Source shows in everyone else's hands, riding the right hand
	 * by bonemerge exactly as the player's third-person shadow carries its own (UpdateWeaponShadow). The
	 * w_ model's hand bone is authored to land on ValveBiped.Bip01_R_Hand; placing the component so it does
	 * needs no invented angles.
	 */
	UPROPERTY(Transient)
	TObjectPtr<USourceStudioModelComponent> WeaponMesh;
	int32 WeaponHandBone = INDEX_NONE;			// the soldier's right hand
	FTransform WeaponRootBind = FTransform::Identity;	// the w_ model's hand, in its own model space
	bool bWeaponBonemerged = false;
	void PlaceHeldWeapon();

	/**
	 * The aim blend. HL2's soldier does not have one "fire" animation: it has a nine-way grid, and where the
	 * gun points inside it is chosen by the aim_pitch and aim_yaw pose parameters. Undriven, the blend sits
	 * at whatever the grid defaults to - which had soldiers firing at the ceiling. The player's own body
	 * drives the same two (ALambdaCharacter::UpdateBodyPose); this is the NPC's half.
	 */
	void UpdateAimPose();
	/** Where the mind last told the body to shoot, in UE space - what the aim blend is pointed at. */
	FVector AimTarget = FVector::ZeroVector;
	bool bHasAimTarget = false;

	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> Voice;
};
