#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "LambdaCharacter.generated.h"

class UCameraComponent;
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

	/** CBasePlayer::FindUseEntity - trace from the eye for a usable entity within PLAYER_USE_RADIUS. */
	AActor* FindUseEntity() const;
	void Input_Quit();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lambda")
	TObjectPtr<UCameraComponent> FirstPersonCamera;

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
	TObjectPtr<UInputAction> QuitAction;

	float WalkSpeedCm = 0.0f;
	float SprintSpeedCm = 0.0f;
};
