#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SourceBSPFile.h"
#include "SourcePropPhysics.generated.h"

class USourceStudioModelComponent;
class UProceduralMeshComponent;
class ULambdaMaterialLibrary;

/**
 * prop_physics (game/server/props.cpp): a model lying in the world that physics owns. Its collision comes from
 * the model's .phy - the same file the ragdolls read - and so do its mass, damping and surface property, which
 * is what decides the sound and decal a bullet leaves on it. Shooting it pushes it (VPhysicsTakeDamage), and the
 * player can pick it up, carry it and throw it with +USE (CPlayerPickupController).
 *
 * Not ported: prop_data (breakable props, their health, gibs and interactions), physics damage from collisions,
 * motion disabling and the constraint spawnflags.
 */
UCLASS()
class LAMBDASOURCE_API ASourcePropPhysics : public AActor
{
	GENERATED_BODY()

public:
	ASourcePropPhysics(const FObjectInitializer& ObjectInitializer);

	/** Reads the keyvalues ("model", "origin", "angles", "massscale") and builds the body. Destroys itself on failure. */
	void InitializeFromEntity(const FSourceEntity& InEntity, ULambdaMaterialLibrary* Materials);

	static bool IsPropClass(const FString& ClassName);

	virtual void Tick(float DeltaSeconds) override;
	virtual void BeginPlay() override;

	/** PhysCollisionSound: a prop landing on something makes the noise its surface property calls for. */
	UFUNCTION()
	void OnBodyHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
		FVector NormalImpulse, const FHitResult& Hit);
	/** CBaseEntity::VPhysicsTakeDamage: the blow's force at the point it landed. */
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

	/** $surfaceprop of the .phy solid ("wood", "metal"), for impact decals and sounds. */
	const FString& GetSurfaceProp() const { return SurfaceProp; }
	float GetMass() const;
	/** The entity's classname, for logs. */
	const FString& GetClassName_Lambda() const { return Entity.ClassName; }
	/** The largest side of the prop's bounds, in Source units (CBasePlayer::CanPickupObject's size limit). */
	float GetSizeUnits() const { return SizeUnits; }
	/**
	 * physcollision->CollideGetExtent: how far the prop reaches from its origin in a direction, as it is turned
	 * now. The grab controller uses this to work out how much room the prop needs to clear the player.
	 */
	float GetExtentAlong(const FVector& Direction) const;
	/** The prop's collision box in its own space, in cm. */
	const FVector& GetHullExtent() const { return HullExtentLocal; }
	UPrimitiveComponent* GetPhysicsBody() const;

private:
	/** CreatePhysicsProp: sets the prop down clear of whatever it is starting inside of, before physics runs. */
	void PlaceClearOfWorld(const FVector3f& HullMin, const FVector3f& HullMax, float Scale);

public:

	// ---- CPlayerPickupController / CGrabController ----

	/** True while a player is carrying this prop. */
	bool IsHeld() const { return Carrier.IsValid(); }
	/** SF_PHYSPROP_PREVENT_PICKUP: the mapper marked this prop as one the player may not pick up. */
	bool IsPickupPrevented() const { return bPickupPrevented; }
	/** AttachEntity: the prop stops falling on its own and follows the carrier. */
	void StartCarry(APawn* Player);
	/** DetachEntity: back to plain physics. bThrown keeps the velocity it was given. */
	void StopCarry(bool bThrown);
	/**
	 * UpdateObject: drives the prop to the point the carrier is holding it at. Returns false when it cannot be
	 * held any more (too far out of place, or something in the way), which is Source's cue to drop it.
	 */
	bool UpdateCarry(const FVector& TargetLocation, const FRotator& TargetRotation, float DeltaSeconds, float MaxErrorUnits);

private:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lambda", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UProceduralMeshComponent> Body;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lambda", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USourceStudioModelComponent> Model;

	UPROPERTY(Transient)
	TObjectPtr<ULambdaMaterialLibrary> MaterialLibrary;

	TWeakObjectPtr<APawn> Carrier;
	/** When the prop last made an impact noise: Source will not play them closer together than 0.05s. */
	float LastImpactSoundTime = 0.0f;
	/**
	 * CGrabController's error tracking. m_errorTime starts at -1: "1 second until error starts accumulating", so
	 * a prop picked up across the room is pulled in rather than dropped for being out of place, and m_error is
	 * then a running average that only trips when the prop is held up for a while.
	 */
	float CarryErrorTime = 0.0f;
	float CarryError = 0.0f;
	bool bPickupPrevented = false;
	FSourceEntity Entity;
	FString SurfaceProp;
	float SizeUnits = 0.0f;
	/** The prop's collision box in its own space, for GetExtentAlong. */
	FVector HullExtentLocal = FVector::ZeroVector;
	/** What the prop really weighs, kept while it is carried at REDUCED_CARRY_MASS. */
	float CarriedMassKg = 0.0f;
	/** Damping the prop had before it was picked up (Source raises it while carried and restores it after). */
	float SavedLinearDamping = 0.0f;
	float SavedAngularDamping = 0.0f;
};
