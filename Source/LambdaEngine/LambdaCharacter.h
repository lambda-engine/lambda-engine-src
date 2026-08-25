#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Gameplay/SourcePlayerPunch.h"
#include "Entities/SourceItemPickup.h"
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
	ALambdaCharacter();

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

	/** CBasePlayer::OnTakeDamage: takes the damage off health (armour is not modelled yet). */
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

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
	 * Dev aid: stamps a row of impact decals on the wall ahead and moves the player to a fixed viewpoint at
	 * DistanceCm from the middle one, looking at it from AngleDeg off its normal - the same framing every run,
	 * which is what tuning a decal's depth by screenshot needs (Source's setpos/setang serve the same purpose).
	 */
	void RunDecalTest(float DistanceCm, float AngleDeg, int32 Count = 3, const FString& DecalName = FString());

	USourceStudioModelComponent* GetViewModelMesh() const { return ViewModelMesh; }

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

	UPROPERTY(EditAnywhere, Category = "Lambda")
	float Health = 100.0f;

	UPROPERTY(EditAnywhere, Category = "Lambda")
	float Armor = 0.0f;
};
