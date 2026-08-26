#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "LambdaPlayerPawn.generated.h"

class UCapsuleComponent;
class ULambdaPlayerMovement;

/**
 * The player: a pawn moved by a ported CGameMovement, wearing a capsule.
 *
 * ACharacter is not the base because its movement component cannot be replaced with Source's move loop - it owns
 * the floor, step and penetration logic itself. So the pawn is plain, and ULambdaPlayerMovement does the moving.
 *
 * The hull is a capsule rather than Source's SOLID_BBOX, and that is a deliberate infidelity. Source sweeps its
 * box against brush planes, which is exact; our world is a triangle mesh, and a box swept against triangle soup
 * catches seam normals at every joint between triangles - which in practice was the player sticking to flat
 * walls. A capsule rolls over the seams. Same trade QMovement makes for the same reason. The capsule matches the
 * box's footprint: radius 16 units, 72 tall standing, 36 ducked.
 */
UCLASS()
class LAMBDAENGINE_API ALambdaPlayerPawn : public APawn
{
	GENERATED_BODY()

public:
	ALambdaPlayerPawn(const FObjectInitializer& ObjectInitializer);

	virtual void BeginPlay() override;
	virtual UPawnMovementComponent* GetMovementComponent() const override;

	UCapsuleComponent* GetHull() const { return Hull; }
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
	/** The player's collision. */
	UPROPERTY(VisibleAnywhere, Category = "Lambda")
	TObjectPtr<UCapsuleComponent> Hull;

	UPROPERTY(VisibleAnywhere, Category = "Lambda")
	TObjectPtr<ULambdaPlayerMovement> Movement;

private:
	void HandleLanded(const FHitResult& Hit);
};
