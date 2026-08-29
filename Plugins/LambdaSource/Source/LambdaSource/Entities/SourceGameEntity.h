#pragma once

#include "CoreMinimal.h"
#include "Entities/SourceBrushEntity.h"
#include "Components/AudioComponent.h"
#include "Game/LambdaGameAPI.h"
#include "SourceGameEntity.generated.h"

/**
 * A brush entity whose behaviour lives in LambdaGame.dll.
 *
 * This is the engine's half of the split, and it is deliberately empty of opinions: it draws, collides,
 * moves and makes noise, and it asks the game module what to do about any of it. Source's engine works the
 * same way - it knows an edict has an origin, a model and a solid type, and has no idea what a func_button
 * is. The class here that would have held the button's logic simply does not exist.
 *
 * Anything the game side cannot claim keeps its native C++ implementation, so entities move across one at a
 * time and a missing DLL costs nothing.
 */
UCLASS()
class LAMBDASOURCE_API ASourceGameEntity : public ASourceBrushEntity
{
	GENERATED_BODY()

public:
	ASourceGameEntity();

	virtual void InitializeFromEntity(const FSourceBSPFile& Map, int32 ModelIndex, const FSourceEntity& InEntity,
		ULambdaMaterialLibrary* MaterialLibrary, ASourceBSPWorldActor* InWorldActor) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	virtual bool IsUsable() const override;
	virtual void OnUsed(AActor* Activator) override;
	virtual bool AcceptInput(const FString& InputName, AActor* Activator, AActor* Caller, const FString& Parameter) override;

	/** CBaseToggle::LinearMove - travel to a Source-space position at a constant speed. */
	void BeginLinearMove(const FVector3f& Destination, float Speed);
	/** CBaseToggle::AngularMove - turn toward a Source-space angle at a constant degrees per second. */
	void BeginAngularMove(const FVector3f& DestinationAngles, float Speed);
	void CancelLinearMove();

	void SetSolidity(bool bSolid);
	/** Makes the brush a volume things pass through, reporting what enters and leaves. */
	void SetTriggerVolume(bool bTrigger);
	void StartLoopingSound(const FString& SoundName);
	void StopLoopingSoundNow();

	FVector GetLocalBoundsSize() const { return LocalBounds.GetSize(); }
	/** SetSourceOrigin is protected on the base; the engine services need it from outside. */
	void SetSourceOriginPublic(const FVector3f& InOrigin) { SetSourceOrigin(InOrigin); }

private:
	lambda::IEntity* Behaviour = nullptr;
	lambda::EntityId GameId = lambda::InvalidEntity;

	bool bMoving = false;
	bool bMovingAngular = false;
	FVector3f MoveTarget = FVector3f::ZeroVector;		// a position, or an angle when bMovingAngular
	float MoveSpeed = 0.0f;

	/** The travel sound, kept so it can be stopped when the move ends rather than running to its own length. */
	UPROPERTY()
	TObjectPtr<UAudioComponent> LoopingSound;

	/** Whatever the mover last found in its way, so OnBlocked keeps being told while it is still there. */
	void CheckBlocked();

	UFUNCTION()
	void HandleBeginOverlap(UPrimitiveComponent* OverlappedComp, AActor* Other, UPrimitiveComponent* OtherComp,
		int32 OtherBodyIndex, bool bFromSweep, const FHitResult& Sweep);
	UFUNCTION()
	void HandleEndOverlap(UPrimitiveComponent* OverlappedComp, AActor* Other, UPrimitiveComponent* OtherComp,
		int32 OtherBodyIndex);

	bool bIsTriggerVolume = false;
};
