#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
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
class LAMBDAENGINE_API ALambdaCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	ALambdaCharacter();

	virtual void BeginPlay() override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	UFUNCTION(BlueprintPure, Category = "Lambda")
	UCameraComponent* GetFirstPersonCamera() const { return FirstPersonCamera; }

	virtual void Tick(float DeltaSeconds) override;

	// ---- Weapons and ammo (CBasePlayer / CBaseCombatCharacter) ----

	/** Spawns a weapon from its script name ("weapon_pistol") and makes it active. */
	UFUNCTION(BlueprintCallable, Category = "Lambda")
	ALambdaWeapon* GiveWeapon(const FString& WeaponClassName);

	UFUNCTION(BlueprintPure, Category = "Lambda")
	ALambdaWeapon* GetActiveWeapon() const { return ActiveWeapon; }

	/** CBasePlayer::GetAmmoCount / GiveAmmo / RemoveAmmo, keyed by the ammo type name from the weapon script. */
	UFUNCTION(BlueprintPure, Category = "Lambda")
	int32 GetAmmoCount(const FString& AmmoType) const;
	int32 GiveAmmo(const FString& AmmoType, int32 Count);
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
	void Input_AttackStart();
	void Input_AttackStop();
	void Input_ReloadStart();
	void Input_ReloadStop();

	/** CBasePlayer::FindUseEntity - trace from the eye for a usable entity within PLAYER_USE_RADIUS. */
	AActor* FindUseEntity() const;
	void Input_Quit();

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
	// lambda.fire.auto state
	int32 AutoFireShotsLeft = 0;
	int32 AutoFireShotTaken = 0;
	float AutoFireInterval = 0.6f;
	float AutoFireTimer = 0.0f;
	float AutoFireScreenshotTimer = 0.0f;
	float AutoFireFinalTimer = 0.0f;
	bool AutoFirePulse = false;
	TWeakObjectPtr<AActor> AutoFireTarget;
	float DecalTestScreenshotTimer = 0.0f;

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

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> ReloadAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> QuitAction;

	float WalkSpeedCm = 0.0f;
	float SprintSpeedCm = 0.0f;

	UPROPERTY(Transient)
	TObjectPtr<ALambdaWeapon> ActiveWeapon;

	/** Ammo carried, by ammo type name. */
	TMap<FString, int32> AmmoCounts;

	UPROPERTY(EditAnywhere, Category = "Lambda")
	float Health = 100.0f;

	UPROPERTY(EditAnywhere, Category = "Lambda")
	float Armor = 0.0f;
};
