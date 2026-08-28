#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Gameplay/SourcePlayerPunch.h"
#include "Entities/SourceItemPickup.h"
#include "LambdaSuitVoice.h"

#include "LambdaCharacter.generated.h"

class UCameraComponent;
class USourceStudioModelComponent;
class UProceduralMeshComponent;
class UPointLightComponent;
class ULambdaMaterialLibrary;
class ALambdaWeapon;
class UInputAction;
class UInputMappingContext;
struct FInputActionValue;

/**
 * First-person player with Half-Life 2 dimensions and movement (capsule 32x72 units, eye height 64, walk 190 u/s,
 * sprint 320 u/s, jump 21 units, gravity 600 u/s^2), scaled by ULambdaSourceSettings::UnitScale.
 * Input is built in code (Enhanced Input) so the project needs no input assets.
 */
UCLASS()
class LAMBDAENGINE_API ALambdaCharacter : public ACharacter, public ISourcePlayerPunch, public ISourceItemPickup
{
	GENERATED_BODY()

public:
	ALambdaCharacter(const FObjectInitializer& ObjectInitializer);

	virtual void BeginPlay() override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	UFUNCTION(BlueprintPure, Category = "Lambda")
	UCameraComponent* GetFirstPersonCamera() const { return FirstPersonCamera; }

	// ISourcePlayerPunch: CBasePlayer::ViewPunch (a sprung kick of the view, DecayPunchAngle) and VelocityPunch.
	virtual void ViewPunch(const FRotator& AngleOffset) override;
	virtual void VelocityPunch(const FVector& VelocityImpulse) override;

	virtual void Tick(float DeltaSeconds) override;
	virtual void NotifyHit(UPrimitiveComponent* MyComp, AActor* Other, UPrimitiveComponent* OtherComp, bool bSelfMoved,
		FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit) override;

	// ---- Weapons and ammo (CBasePlayer / CBaseCombatCharacter) ----

	/** Spawns a weapon from its script name ("weapon_pistol") and makes it active. */
	UFUNCTION(BlueprintCallable, Category = "Lambda")
	ALambdaWeapon* GiveWeapon(const FString& WeaponClassName);

	UFUNCTION(BlueprintPure, Category = "Lambda")
	ALambdaWeapon* GetActiveWeapon() const { return ActiveWeapon; }
	/** Every weapon the player carries, for the selection HUD (sorted by bucket, then position). */
	const TArray<TObjectPtr<ALambdaWeapon>>& GetWeapons() const { return Weapons; }
	ALambdaWeapon* FindWeapon(const FString& WeaponClassName) const;
	/** CBasePlayer::Weapon_Switch: holster the old, deploy the new. */
	void SwitchToWeapon(ALambdaWeapon* Weapon);

	/** The weapon selection menu (CHudWeaponSelection's state lives on the player here). */
	bool IsWeaponSelectionActive() const { return bSelectionActive; }
	ALambdaWeapon* GetSelectedWeapon() const { return SelectionIndex >= 0 && Weapons.IsValidIndex(SelectionIndex) ? Weapons[SelectionIndex].Get() : nullptr; }

	/** What the HUD's damage indicator needs: when, how hard, and from which side. */
	float GetLastDamageTime() const { return LastDamageTime; }
	float GetLastDamageAmount() const { return LastDamageAmount; }
	/** Angle of the blow relative to the view: 0 ahead, +90 right, -90 left, +/-180 behind. */
	float GetLastDamageYaw() const { return LastDamageYaw; }
	/** m_bitsDamageType: what kinds of harm are currently being done, for the HUD's damage-type icons. */
	int32 GetDamageBits() const { return DamageBits; }
	float GetDamageBitsTime() const { return DamageBitsTime; }

	/** CHudHistoryResource: the last few pickups, newest last. */
	struct FPickupEvent { FString Text; float Time = 0.0f; };
	const TArray<FPickupEvent>& GetPickupHistory() const { return PickupHistory; }

	/** CBasePlayer::GetAmmoCount / GiveAmmo / RemoveAmmo, keyed by the ammo type name from the weapon script. */
	UFUNCTION(BlueprintPure, Category = "Lambda")
	int32 GetAmmoCount(const FString& AmmoType) const;
	virtual int32 GiveAmmo(const FString& AmmoType, int32 Count) override;
	/** CBasePlayer::BumpWeapon: takes a weapon off the floor, or just its ammo when it is already carried. */
	virtual bool BumpWeapon(const FString& WeaponClassName) override;
	void RemoveAmmo(const FString& AmmoType, int32 Count);

	/**
	 * CBasePlayer::OnTakeDamage: armour first, then health, then the suit says what it makes of it.
	 *
	 * The damage type matters here, not just the amount - a fall and a bullet of the same size are different
	 * injuries, and the HEV suit names them differently. FSourceDamageEvent carries the DMG_* bits; a plain
	 * UE damage event arrives as DMG_GENERIC and is simply survived quietly.
	 */
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

	/** CBasePlayer::Event_Killed: health has run out. */
	void Killed(AActor* Attacker);
	bool IsAlive() const { return Health > 0.0f; }

	/** The HEV suit's voice, so items and the world can make it say things. */
	FLambdaSuitVoice& GetSuitVoice() { return SuitVoice; }
	/** CBasePlayer::EquipSuit - without it the suit neither speaks nor shows armour. */
	void EquipSuit(bool bEquip = true);
	bool IsSuitEquipped() const { return bSuitEquipped; }

	/** IncrementArmorValue, capped at 100 as Source caps it. */
	void GiveArmor(float Amount);

protected:
	/** The suit's half of OnTakeDamage: which injury this was, and what that leaves the player. */
	void SuitDamageReaction(int32 DamageType, float Damage, float HealthPrev);

public:

	/** Builds the first-person view model from a Source .mdl and shows it on the camera. */
	UFUNCTION(BlueprintCallable, Category = "Lambda")
	bool SetViewModel(const FString& ModelPath);

	/** CBaseViewModel::SendViewModelAnim - plays the sequence matching a Source activity ("ACT_VM_IDLE", ...). */
	bool SendViewModelAnim(const FString& ActivityName);

	/** CBasePlayer::DoMuzzleFlash - flashes the view model's "muzzle" attachment. */
	void DoMuzzleFlash();

	/** CC_NPC_Create: spawns the named NPC at the crosshair trace point (or MaxDistanceCm ahead if nearer), facing the player. */
	AActor* NPCCreate(const FString& ClassName, float MaxDistanceCm = 5000.0f);
	/** prop_physics_create: drops a physics prop of that model where the player is looking. */
	AActor* PropCreate(const FString& ModelPath, float MaxDistanceCm = 5000.0f);

	/** The material library the loaded map built, used to resolve $surfaceprop at a bullet impact. */
	ULambdaMaterialLibrary* GetWorldMaterialLibrary() const;

	/**
	 * CBasePlayer::UpdateStepSound: decides whether this frame is a footfall, and if so what it lands on.
	 * Steps are timed rather than driven by the animation - Source counts down a few hundred milliseconds
	 * between them, shorter when running.
	 */
	/** Eases the eye between the standing and ducked view heights, as Source does over TIME_TO_DUCK. */
	void UpdateEyeHeight(float DeltaSeconds);
	void UpdateStepSound(float DeltaSeconds);
	/** CBasePlayer::PlayStepSound: the surface names the sound, alternating feet. */
	void PlayStepSound(const FString& SurfaceProp, float Volume);

	/**
	 * A soundscript played flat in the player's ears rather than out in the world.
	 *
	 * The selection sounds are SNDLVL_NONE in the scripts, which is Source's way of saying this one is not in
	 * the world at all - no attenuation, no direction, no falloff. Emitting it at the player's location would
	 * still pan it as they turned.
	 */
	void PlayUISound(const FString& ScriptName);

	/** m_flStepSoundTime, in milliseconds, exactly as Source keeps it. */
	float StepSoundTime = 0.0f;
	/** m_nStepside. */
	bool bStepSide = false;

	/**
	 * Dev aid: stamps a row of impact decals on the wall ahead and moves the player to a fixed viewpoint at
	 * DistanceCm from the middle one, looking at it from AngleDeg off its normal - the same framing every run,
	 * which is what tuning a decal's depth by screenshot needs (Source's setpos/setang serve the same purpose).
	 */
	void RunDecalTest(float DistanceCm, float AngleDeg, int32 Count = 3, const FString& DecalName = FString());

	USourceStudioModelComponent* GetViewModelMesh() const { return ViewModelMesh; }

	// ---- The player's own body (LambdaPlayerBody.cpp) ----

	/** Loads the legs and the shadow body and sets their visibility rules. Called once the map's materials exist. */
	void SetupPlayerBody();
	/** Picks the sequence both meshes play from how the player is moving, and keeps their feet on the ground. */
	void UpdatePlayerBody(float DeltaSeconds);
	/** The sequence label the current movement calls for ("run_forward", "crouch_idle", ...). */
	FString ChoosePlayerBodySequence() const;
	/** The hl2mp activity the current movement calls for ("ACT_HL2MP_RUN_SHOTGUN", ...). */
	FString ChoosePlayerBodyActivity() const;
	/** Which hl2mp pose family the active weapon carries as ("SHOTGUN", "PISTOL", ...). */
	FString WeaponActivitySuffix() const;
	/** The shadow body plays its attack or reload gesture; called by the weapon when it fires or reloads. */
	void OnWeaponAttackAnim();
	void OnWeaponReloadAnim();
	void PlayBodyGesture(const TCHAR* GesturePrefix);
	/** Keeps the active weapon's world model in the shadow body's hand. */
	void UpdateWeaponShadow(USourceStudioModelComponent* Body);

public:
	/**
	 * CAM_ToThirdPerson / CAM_ToFirstPerson: which side of his own eyes the player is on.
	 *
	 * In first person the body is drawn for everyone except its owner and casts anyway, so the player sees his
	 * shadow but not himself, and a legs-only copy fills in what looking down should show. In third person
	 * there is nothing to fake: the body is simply drawn, the legs copy is switched off, and the camera backs
	 * away along the view.
	 */
	void SetThirdPerson(bool bEnable);

	/**
	 * cl_playermodel: swaps the model the player wears, without a restart.
	 *
	 * Both meshes change together - they are the same character seen two ways, and a shadow and a pair of legs
	 * that disagree about whose legs they are would be worse than either alone. The body and the legs are
	 * rebuilt from scratch, because the legs mesh is cut at a bone when it is built, not hidden afterwards.
	 */
	void SetPlayerModel(const FString& ModelPath);
	bool IsThirdPerson() const { return bThirdPerson; }

protected:
	/** Pulls the camera back to cam_idealdist behind the eye, stopping short of whatever is in the way. */
	void UpdateCameraDistance();
	bool bThirdPerson = false;
	/** What the body last posed as, so a duck begun in mid air can change the pose in one frame. */
	bool bLastBodyDucked = false;

public:
	/** Freezes the shadow body's arms into a two-handed carry, solved from the model's own bind skeleton. */
	void SolveHoldPose(USourceStudioModelComponent* Body);

	UFUNCTION(BlueprintPure, Category = "Lambda") float GetHealth() const { return Health; }
	UFUNCTION(BlueprintPure, Category = "Lambda") float GetArmor() const { return Armor; }

protected:
	void ApplySourceMovementSettings();
	void BuildInputAssets();
	void AddMappingContextToPlayer();

	void Input_Move(const FInputActionValue& Value);
	void Input_Look(const FInputActionValue& Value);
	void Input_JumpStart();
	void Input_JumpEnd();
	void Input_SprintStart();
	void Input_SprintEnd();
	/** IN_DUCK. Source ducks while the key is held and stands again when it is let go. */
	void Input_CrouchStart();
	void Input_CrouchEnd();

public:
	/**
	 * CGameMovement::FinishDuck / FinishUnDuck.
	 *
	 * Unreal keeps the feet where they are when the capsule shrinks, so ducking never buys any height. Source
	 * does that only when standing on something: in the air it moves the origin up by the difference between the
	 * hulls, lifting the feet and leaving the head where it was. That is crouch jumping, and it is the reason a
	 * Half-Life player can reach a ledge a plain jump cannot.
	 */
	virtual void OnStartCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust) override;
	virtual void OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust) override;

	/**
	 * CGameMovement::CheckJumpButton: a ducked player may jump.
	 *
	 * Unreal forbids it outright - CanJumpInternal_Implementation is "return !IsCrouched() && ...". Source only
	 * refuses while the unduck transition is still running, and has a branch specifically for jumping while
	 * ducked, which sets the jump velocity rather than adding to it.
	 */
	virtual bool CanJumpInternal_Implementation() const override;

protected:
	void Input_Use();
	/** CPlayerPickupController: picks up the physics prop in front of the player, or drops the carried one. */
	void TogglePropCarry();
	/** CGrabController::UpdateObject: keeps the carried prop in front of the player. */
	void UpdatePropCarry(float DeltaSeconds);
	/**
	 * The player's physics shadow pushing what he walks into. Source gives that shadow a push mass limit of 350 kg
	 * and a push speed limit of 50 units/s, so walking into a crate shifts it at walking pace and no faster.
	 */
	void PushPhysicsObject(const FHitResult& Hit);

	/** The 2D diagonal of the player's own bounds, which a carried prop has to clear. */
	float PlayerHullRadiusCm() const;

	/** Shutdown(): lets go of the carried prop and puts the weapon back. */
	void DropCarriedProp(bool bThrown);
	/** Throws the carried prop (player_throwforce, scaled by its mass). Returns false when nothing is carried. */
	bool ThrowCarriedProp();
	void Input_AttackStart();
	void Input_Attack2Start();
	void Input_Attack2Stop();
	void Input_AttackStop();
	void Input_ReloadStart();
	void Input_ReloadStop();

	/** CBasePlayer::FindUseEntity - trace from the eye for a usable entity within PLAYER_USE_RADIUS. */
	AActor* FindUseEntity() const;
	void Input_Quit();
	/** slot1..slot5 / lastinv / invnext / invprev, the HL2 weapon selection commands. */
	void Input_Slot1() { SelectSlot(0); }
	void Input_Slot2() { SelectSlot(1); }
	void Input_Slot3() { SelectSlot(2); }
	void Input_Slot4() { SelectSlot(3); }
	void Input_Slot5() { SelectSlot(4); }
	void Input_LastInv();
	void Input_InvNext() { CycleSelection(+1); }
	void Input_InvPrev() { CycleSelection(-1); }
	void SelectSlot(int32 Bucket);
	/** CBaseHudWeaponSelection::SelectWeapon: take what the menu is on and close it. */
	void ConfirmWeaponSelection();

public:
	/** Opens the weapon selection on a bucket and leaves it open, which is what the scroll wheel does. */
	void OpenWeaponSelection(int32 Bucket) { SelectSlot(Bucket); }

protected:
	void CycleSelection(int32 Step);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lambda")
	TObjectPtr<UCameraComponent> FirstPersonCamera;

	/** The first-person weapon model, parented to the camera. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lambda")
	TObjectPtr<USourceStudioModelComponent> ViewModelMesh;

	UPROPERTY(Transient)
	TObjectPtr<ULambdaMaterialLibrary> ViewModelMaterials;

	/** Quads for the muzzle flash sprite, in the camera's space (FX_MuzzleEffect). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lambda")
	TObjectPtr<UProceduralMeshComponent> MuzzleFlashMesh;

	/** The elight ProcessMuzzleFlashEvent allocates at the muzzle. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lambda")
	TObjectPtr<UPointLightComponent> MuzzleFlashLight;

	/** World times at which the flash sprite and its light are switched off again. */
	float MuzzleFlashSpriteDieTime = 0.0f;
	float MuzzleFlashLightDieTime = 0.0f;
	float MuzzleFlashLightRadius = 0.0f;
	float AutoCommandTimer = 0.0f;
	bool bAutoCommandsRun = false;
	// fire.auto state
	int32 AutoFireShotsLeft = 0;
	int32 AutoFireShotTaken = 0;
	float AutoFireInterval = 0.6f;
	float AutoFireTimer = 0.0f;
	float AutoFireScreenshotTimer = 0.0f;
	float AutoFireFinalTimer = 0.0f;
	bool AutoFirePulse = false;
	/** firehold.auto: how much longer to keep the trigger down, and how long until it goes down. */
	float AutoFireHoldLeft = 0.0f;
	float AutoFireHoldDelay = 0.0f;
	bool bAutoFireHolding = false;
	bool bAutoFireHoldAlt = false;
	TWeakObjectPtr<AActor> AutoFireTarget;
	/** The prop the player is carrying (CPlayerPickupController's grab controller), and how it is held. */
	TWeakObjectPtr<class ASourcePropPhysics> CarriedProp;
	FRotator CarriedPropRelativeRotation = FRotator::ZeroRotator;

	/** propcarry.auto state: seconds until the prop is grabbed, and until it is thrown. */
	float AutoCarryGrabTimer = -1.0f;
	float AutoCarryThrowTimer = -1.0f;
	float AutoCarryLookPitch = 0.0f;
	/** pitchsweep.auto state. */
	float SweepFromPitch = 0.0f;
	float SweepToPitch = 0.0f;
	float SweepSeconds = 0.0f;
	float SweepDelay = 0.0f;
	float SweepElapsed = 0.0f;
	/** The scripted view (setpos.auto) is held briefly, and scripted spawns fire inside that window. */
	FRotator AutoViewRotation = FRotator::ZeroRotator;
	float AutoViewHoldSeconds = 0.0f;
	float AutoSpawnDelay = 0.0f;
	FString PendingNPCCreate;
	FString PendingPropCreate;
	/** slot.auto state. */
	int32 AutoSlotBucket = 0;
	float AutoSlotDelay = 0.0f;
	/** walk.auto state. */
	float AutoWalkSeconds = 0.0f;
	float AutoWalkDelay = 0.0f;
	/**
	 * How high the eye sits above the feet right now, in centimetres, easing towards where ducking wants it.
	 *
	 * Source moves the view offset over TIME_TO_DUCK rather than switching it (CGameMovement::Duck), which is
	 * what makes ducking read as crouching rather than as the camera changing places.
	 */
	float EyeAboveFeetCm = -1.0f;

	float AutoCrouchSeconds = 0.0f;
	float AutoCrouchDelay = 0.0f;
	float AutoSpeedLogSeconds = 0.0f;
	float AutoThirdPersonDelay = 0.0f;
	float AutoFirstPersonDelay = 0.0f;
	float AutoHurtAmount = 0.0f;
	float AutoHurtDelay = 0.0f;
	FString AutoHurtType;
	float AutoSpeedLogTimer = 0.0f;
	bool bAutoJumpArmed = false;
	float AutoJumpDelay = 0.0f;
	float AutoJumpDuckAfter = -1.0f;
	float AutoJumpElapsed = 0.0f;
	float AutoJumpStartFeetZ = 0.0f;
	float AutoJumpPeakFeetZ = 0.0f;
	float AutoJumpPeakAirSpeed = 0.0f;
	float AutoJumpSpeedAtLaunch = 0.0f;
	float DecalTestScreenshotTimer = 0.0f;
	bool bAutoFireAimHead = false;
	/** m_Local.m_vecPunchAngle / m_vecPunchAngleVel (pitch, yaw, roll degrees). */
	FVector PunchAngle = FVector::ZeroVector;
	FVector PunchAngleVel = FVector::ZeroVector;
	void DecayPunchAngle(float DeltaSeconds);

	/** Frames the flash is held visible regardless of its die time, so a sub-frame lifetime still renders once. */
	int32 MuzzleFlashHoldFrames = 0;

	void UpdateMuzzleFlash();

	/** Cached so a bullet impact does not search the level for the BSP actor on every shot. */
	UPROPERTY(Transient)
	mutable TObjectPtr<ULambdaMaterialLibrary> WorldMaterialLibrary;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> MappingContext;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> MoveAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> LookAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> JumpAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> SprintAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> CrouchAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> UseAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> AttackAction;

	/** IN_ATTACK2 - the shotgun's both barrels, and whatever else takes a right click. */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> Attack2Action;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> ReloadAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> QuitAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> SlotActions[5];
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> LastInvAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> InvNextAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> InvPrevAction;

	float WalkSpeedCm = 0.0f;
	float SprintSpeedCm = 0.0f;

	UPROPERTY(Transient)
	TObjectPtr<ALambdaWeapon> ActiveWeapon;

	UPROPERTY(Transient)
	TArray<TObjectPtr<ALambdaWeapon>> Weapons;
	UPROPERTY(Transient)
	TObjectPtr<ALambdaWeapon> LastWeapon;

	/** Weapon selection menu state. */
	bool bSelectionActive = false;
	int32 SelectionIndex = -1;

	/** Damage indicator memory. */
	float LastDamageTime = -1000.0f;
	float LastDamageAmount = 0.0f;
	float LastDamageYaw = 0.0f;

	TArray<FPickupEvent> PickupHistory;

	/** Ammo carried, by ammo type name. */
	TMap<FString, int32> AmmoCounts;

	/** Drawn only for the player who owns them: what looking down shows. */
	UPROPERTY(VisibleAnywhere, Category = "Lambda")
	TObjectPtr<USourceStudioModelComponent> LegsMesh;
	/** Drawn for everyone else, and casting even when hidden: the shadow the player throws. */
	UPROPERTY(VisibleAnywhere, Category = "Lambda")
	TObjectPtr<USourceStudioModelComponent> BodyMesh;
	/** The active weapon's world model, in the shadow body's hand, so the shadow is armed too. */
	UPROPERTY(VisibleAnywhere, Category = "Lambda")
	TObjectPtr<USourceStudioModelComponent> WeaponShadowMesh;
	/** The class whose world model the shadow is holding, so a weapon switch reloads it. */
	FString WeaponShadowClass;
	/** The shadow body's right-hand bone, found once per model. */
	int32 WeaponShadowBone = -1;
	/**
	 * The world model's own hand bone in its bind pose, cached per weapon.
	 *
	 * A w_ model is built to be bonemerged: its root bone is named for the hand it belongs in and its geometry
	 * hangs off ValveBiped.Weapon_bone at the offset Valve authored. Undoing this transform is what puts that
	 * root onto the player's hand, and with it the gun exactly where it was meant to sit.
	 */
	FTransform WeaponShadowRootBind = FTransform::Identity;
	bool bWeaponShadowBonemerged = false;

	UPROPERTY(EditAnywhere, Category = "Lambda")
	float Health = 100.0f;

	UPROPERTY(EditAnywhere, Category = "Lambda")
	float Armor = 0.0f;

	/** m_bitsDamageType and when it was last added to, so the HUD can show and then drop the icons. */
	int32 DamageBits = 0;
	float DamageBitsTime = 0.0f;

	bool bSuitEquipped = true;	// the player starts suited; there is no HEV pickup to find yet
	FLambdaSuitVoice SuitVoice;
};
