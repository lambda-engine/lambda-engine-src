#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "LambdaPlayerPawn.generated.h"

class UBoxComponent;
class ULambdaPlayerMovement;

/**
 * The player, as a box.
 *
 * ACharacter would be the obvious base and cannot be used: it builds a UCapsuleComponent in its own constructor
 * and its movement component asks for that capsule everywhere it works out a floor, a step or a penetration.
 * Source's player is SOLID_BBOX - (-16,-16,0) to (16,16,72) standing, 36 tall ducked - and the difference shows
 * wherever an edge is involved, so the shape had to come first and the base class second.
 *
 * What is left of ACharacter's job lives here: somewhere to put the hull, the crouch and jump the game asks for,
 * and a landing to tell people about. The moving itself is ULambdaPlayerMovement.
 */
UCLASS()
class LAMBDAENGINE_API ALambdaPlayerPawn : public APawn
{
	GENERATED_BODY()

public:
	ALambdaPlayerPawn(const FObjectInitializer& ObjectInitializer);

	virtual void BeginPlay() override;
	virtual UPawnMovementComponent* GetMovementComponent() const override;

	UBoxComponent* GetHull() const { return Hull; }
	ULambdaPlayerMovement* GetMovement() const { return Movement; }

	/** Half the hull's height in centimetres - what GetScaledCapsuleHalfHeight used to answer. */
	float GetHullHalfHeight() const;
	/** The bottom of the hull. */
	FVector GetFeetLocation() const;

	// ---- what ACharacter used to provide ----
	UFUNCTION(BlueprintCallable, Category = "Lambda") void Jump();
	UFUNCTION(BlueprintCallable, Category = "Lambda") void Crouch();
	UFUNCTION(BlueprintCallable, Category = "Lambda") void UnCrouch();
	UFUNCTION(BlueprintPure, Category = "Lambda") bool IsCrouched() const;

	/** Called when the player lands, the way ACharacter::Landed was. */
	virtual void Landed(const FHitResult& Hit) {}

protected:
	/** The player's collision: a box, because Source's is. */
	UPROPERTY(VisibleAnywhere, Category = "Lambda")
	TObjectPtr<UBoxComponent> Hull;

	UPROPERTY(VisibleAnywhere, Category = "Lambda")
	TObjectPtr<ULambdaPlayerMovement> Movement;

private:
	void HandleLanded(const FHitResult& Hit);
};
