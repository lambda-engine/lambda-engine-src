#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "SourceBSPFile.h"
#include "SourceDamage.h"
#include "SourceNPCBase.generated.h"

struct FSourceHitboxHit;

class USourceStudioModelComponent;
class ULambdaMaterialLibrary;
class ASourceBSPWorldActor;
class ASourceRagdoll;

/** BLOOD_COLOR_* from shareddefs.h. */
UENUM()
enum class ESourceBloodColor : uint8
{
	DontBleed,
	Red,
	Yellow,
	Green,
	Mech,
	Zombie		// BLOOD_COLOR_ZOMBIE (HL2_EPISODIC): red spray, non-red decals
};

/** NPC_STATE_* from ai_npcstate.h, the subset a combat NPC moves through. */
UENUM()
enum class ESourceNPCState : uint8
{
	None,
	Idle,
	Alert,
	Combat,
	Dead
};

/**
 * The parts of CAI_BaseNPC (game/server/ai_basenpc.cpp) a single hostile NPC needs: a studio model that animates
 * through activities, a hull that walks, health and death, sensing (field of view + line of sight), yaw control,
 * soundscript emitters, and a 0.1s think.
 *
 * It is an ACharacter rather than an ASourceEntity because UE's character movement component - ground following,
 * steps, launches - is tied to ACharacter, and that is the right tool for a hull that walks and leaps. Entity
 * keyvalues are kept alongside; input/output plumbing for NPCs is not wired yet.
 *
 * What is deliberately not here: the schedule/task/condition system, squads, the node graph and pathfinding.
 * Derived NPCs port the schedules they actually use as explicit think logic, and move in straight lines.
 */
UCLASS(Abstract)
class LAMBDASOURCE_API ASourceNPCBase : public ACharacter
{
	GENERATED_BODY()

public:
	ASourceNPCBase(const FObjectInitializer& ObjectInitializer);

	/** Reads keyvalues, runs Spawn(), places the NPC. Called right after spawning, before BeginPlay. */
	virtual void InitializeFromEntity(const FSourceEntity& InEntity, ASourceBSPWorldActor* InWorldActor, ULambdaMaterialLibrary* Materials);

	virtual void Tick(float DeltaSeconds) override;
	/** The NPC's physics shadow pushing props out of its way (ASourcePropPhysics::ShadowPush). */
	virtual void NotifyHit(UPrimitiveComponent* MyComp, AActor* Other, UPrimitiveComponent* OtherComp, bool bSelfMoved,
		FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit) override;
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

	/** $surfaceprop of the model, for impact decals and sounds (a headcrab is "alienflesh"). */
	const FString& GetSurfaceProp() const;
	const FString& GetClassName() const { return Entity.ClassName; }
	ESourceNPCState GetNPCState() const { return NPCState; }
	float GetHealth() const { return Health; }
	/** CBaseCombatCharacter::BloodColor. */
	ESourceBloodColor GetBloodColor() const { return BloodColor; }
	/** The ragdoll this NPC became on death, if any. */
	ASourceRagdoll* GetRagdoll() const { return Ragdoll.Get(); }
	ASourceBSPWorldActor* GetWorldActor() const { return WorldActor.Get(); }
	USourceStudioModelComponent* GetModelComponent() const { return Model; }

	/** Bullets hit hitboxes, not the hull (TraceToStudio); false when the model has no hitboxes. */
	bool HasHitboxes() const;
	bool TraceHitboxes(const FVector& Start, const FVector& End, FSourceHitboxHit& OutHit) const;
	/** CAI_BaseNPC::GetHitgroupDamageMultiplier: sk_npc_head etc. from skill.cfg. */
	virtual float GetHitgroupDamageMultiplier(int32 HitGroup, const FSourceDamageEvent& Info) const;
	int32 GetLastHitGroup() const { return LastHitGroup; }

	/**
	 * CreateRagGib: the NPC dies on the spot without a sound and becomes a ragdoll with this impulse, gone after
	 * Lifetime seconds (how a zombie's headcrab comes off dead).
	 */
	void BecomeRagGib(const FVector& ForceImpulse, const FVector& ForcePosition, float Lifetime);
	bool IsAlive() const { return NPCState != ESourceNPCState::Dead; }

	// ---- CAI_BaseNPC-style helpers, all in UE space unless named "Units" ----

	/** CBaseCombatCharacter::EyePosition: origin plus the view offset set in Spawn. */
	FVector EyePosition() const;
	/** CBaseEntity::FVisible: a clear line from our eyes to the target's eyes (or centre). */
	bool FVisible(const AActor* Target) const;
	/** CBaseCombatCharacter::FInViewCone: the point is within FieldOfView (a cosine) of our facing. */
	bool FInViewCone(const FVector& WorldPos) const;
	/** Distance to an actor's origin, in Source units. */
	float DistanceUnits(const AActor* Target) const;
	/** Our feet (Source origin), in UE space. */
	FVector GetFeetLocation() const;

	/** CBaseEntity::EmitSound for a soundscript name, at our position. */
	void EmitSound(const FString& SoundScript);

	/** SetIdealActivity: picks a sequence for the activity and plays it; a looping activity already playing is left alone. */
	bool SetActivity(const FString& ActivityName);
	const FString& GetActivity() const { return CurrentActivity; }
	bool HaveSequenceForActivity(const FString& ActivityName) const;
	/** CBaseAnimating::IsActivityFinished. */
	bool IsActivityFinished() const;

	/** CAI_Motor yaw: the NPC turns toward IdealYaw at MaxYawSpeed each tick. */
	void SetIdealYaw(float YawDegrees) { IdealYaw = YawDegrees; }
	void SetIdealYawToTarget(const FVector& WorldPos);
	float DeltaIdealYaw() const;
	bool FacingIdeal() const { return FMath::Abs(DeltaIdealYaw()) <= 10.0f; }

	bool IsOnGround() const;
	APawn* GetPlayerPawn() const;
	/**
	 * CAI_Motor: the hull is being pushed but is not moving (a wall, a door, a corner). Source's AI fails the
	 * route and picks another schedule; ours stops shoving the model into the obstruction, which is what makes a
	 * hunched NPC lean through a thin door.
	 */
	bool IsMovementBlocked() const { return bMovementBlocked; }

	/**
	 * CAI_Motor: the direction the NPC is moving in, held until changed. UE's movement component consumes input per
	 * frame, so a think-rate request would brake between thinks; Tick re-applies this every frame. Zero stops.
	 */
	void SetMoveDirection(const FVector& WorldDir) { MoveDirection = WorldDir.GetSafeNormal2D(); }
	void StopMoving();

protected:
	/** Per-NPC spawn: model, hull, health, view offset (CBaseNPC::Spawn). */
	virtual void Spawn() {}
	/** CAI_BaseNPC::NPCThink, every ThinkInterval seconds while alive. */
	virtual void NPCThink() {}
	/** CBaseAnimating::HandleAnimEvent for this model's sequence events. */
	virtual void HandleAnimEvent(int32 EventId, const FString& EventName, const FString& Options);
	/** CAI_BaseNPC::TraceAttack hook, called with the hit group before the damage lands (a zombie notes head shots here). */
	virtual void TraceAttack(const FSourceDamageEvent& Info) {}
	/**
	 * CBaseCombatCharacter::OnTakeDamage_Alive: the base applies Damage to Health and flinches; overrides scale the
	 * damage first and call Super, exactly as the Source classes chain.
	 */
	virtual void OnTakeDamage_Alive(float Damage, AActor* Attacker, const FSourceDamageEvent& Info);
	/** CAI_BaseNPC::IsHeavyDamage: more than 20 points. */
	virtual bool IsHeavyDamage(float Damage, const FSourceDamageEvent& Info) const { return Damage > 20.0f; }
	/** CAI_BaseNPC::GetDeathActivity, reduced to the hit group: the death animation the model has for the wound. */
	FString GetDeathActivity() const;
	/** CAI_BaseNPC::PlayFlinchGesture / GetFlinchActivity / CanFlinch. */
	void PlayFlinchGesture();
	FString GetFlinchActivity(bool bHeavyDamage, bool bGesture) const;
	bool CanFlinch() const;
	/** CBaseCombatCharacter::Event_Killed. */
	virtual void Event_Killed(AActor* Attacker);
	/**
	 * CBaseCombatCharacter::BecomeRagdoll: hands the body to physics using the model's .phy, kicked by the force
	 * of the killing blow. False when the model has no collision model.
	 */
	virtual bool BecomeRagdoll(const FVector& ForceImpulse, const FVector& ForcePosition);
	/** CBaseCombatCharacter::CalcDamageForceVector: the blow's own force, or one made up from the attacker. */
	FVector CalcDamageForceVector(float Damage, const FVector& GivenForce, AActor* Attacker) const;

	virtual void IdleSound() {}
	virtual void AlertSound() {}
	virtual void PainSound() {}
	virtual void DeathSound() {}
	/** CAI_BaseNPC::ShouldPlayIdleSound: a 1-in-100 chance per think while idle or alert. */
	virtual bool ShouldPlayIdleSound() const;
	/** Called the moment the hull gets stuck against something while moving. */
	virtual void OnMovementBlocked() {}

	/** Loads the studio model onto the mesh component. */
	bool SetModel(const FString& ModelPath);
	/** SetHullType/SetHullSizeNormal: a box hull in Source units, mapped onto the capsule. */
	void SetHull(float HalfWidthUnits, float HeightUnits);
	/** Updates the yaw toward IdealYaw (CAI_Motor::UpdateYaw). */
	void UpdateYaw(float DeltaSeconds);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lambda")
	TObjectPtr<USourceStudioModelComponent> Model;

	UPROPERTY(Transient)
	TObjectPtr<ULambdaMaterialLibrary> MaterialLibrary;

	FSourceEntity Entity;
	TWeakObjectPtr<ASourceBSPWorldActor> WorldActor;

	ESourceNPCState NPCState = ESourceNPCState::None;
	float Health = 0.0f;
	float MaxHealth = 0.0f;
	/** SetBloodColor in Spawn. */
	ESourceBloodColor BloodColor = ESourceBloodColor::Red;

	/** The last CTakeDamageInfo's physics side, for the ragdoll. */
	FVector LastDamageForce = FVector::ZeroVector;		// kg*cm/s
	FVector LastDamagePosition = FVector::ZeroVector;
	TWeakObjectPtr<ASourceRagdoll> Ragdoll;
	int32 LastHitGroup = 0;				// m_LastHitGroup
	bool bDyingWithAnim = false;		// playing a death animation; the ragdoll comes when it ends
	float NextFlinchTime = 0.0f;		// m_flNextFlinchTime
	bool bFlinchedMemory = false;		// bits_MEMORY_FLINCHED

	/** m_flFieldOfView: cosine of the half-angle; 0.5 is 120 degrees. */
	float FieldOfView = 0.5f;
	/** CAI_Senses look distance, Source units (SetDistLook default 2048). */
	float LookDistUnits = 2048.0f;
	/** CAI_BaseNPC::MaxYawSpeed default. */
	float MaxYawSpeedDeg = 45.0f;
	/** SetViewOffset, Source units. */
	FVector3f ViewOffsetUnits = FVector3f(0, 0, 0);
	/** How long a corpse stays before it is removed (Source ragdolls are recycled by count, not time). */
	float CorpseLifetime = 30.0f;

	/** CAI_BaseNPC thinks ten times a second. */
	static constexpr float ThinkInterval = 0.1f;

private:
	void OnModelAnimationEvent(int32 EventId, const FString& EventName, const FString& Options);

	FString CurrentActivity;
	FVector MoveDirection = FVector::ZeroVector;
	float IdealYaw = 0.0f;
	float ThinkAccumulator = 0.0f;
	float HullHalfHeightCm = 0.0f;
	float BlockedTime = 0.0f;		// how long the hull has been pushed without moving
	bool bMovementBlocked = false;
};
