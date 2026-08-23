#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "LambdaCharacter.generated.h"

class UCameraComponent;
class UProceduralMeshComponent;
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

	/** Builds the first-person view model from a Source .mdl and shows it on the camera. */
	UFUNCTION(BlueprintCallable, Category = "Lambda")
	bool SetViewModel(const FString& ModelPath);

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
	TObjectPtr<UProceduralMeshComponent> ViewModelMesh;

	UPROPERTY(Transient)
	TObjectPtr<ULambdaMaterialLibrary> ViewModelMaterials;

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
