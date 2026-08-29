#pragma once

#include "CoreMinimal.h"
#include "World/SourceEntity.h"
#include "Game/LambdaGameAPI.h"
#include "SourceGamePointEntity.generated.h"

class ULocalLightComponent;
class ULambdaMaterialLibrary;

/**
 * A point entity whose behaviour lives in LambdaGame.dll - the counterpart to ASourceGameEntity, which does
 * the same job for brush entities.
 *
 * Two hosts rather than one because a point entity has no brush model: the brush host inherits the geometry
 * building, and a light has nothing to build. What they share is the forwarding, which is short enough that
 * keeping them apart costs less than a shared base would.
 *
 * The light component is created only for the classnames that want one. Everything the light *looks* like -
 * colour, falloff, units, whether it casts shadows - is settled here from the map's keyvalues, exactly as it
 * was before; all the game gets to say is how bright it should be at this instant.
 */
UCLASS()
class LAMBDASOURCE_API ASourceGamePointEntity : public ASourceEntity
{
	GENERATED_BODY()

public:
	ASourceGamePointEntity();

	/** Builds whatever this classname needs (a light, so far), then hands it to the game module. */
	void InitializeGameEntity(const FSourceEntity& InEntity, ASourceBSPWorldActor* InWorldActor,
		ULocalLightComponent* ConfiguredLight);

	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual bool AcceptInput(const FString& InputName, AActor* Activator, AActor* Caller, const FString& Parameter) override;

	/** Creates the light component for a light/light_spot, before the caller configures it. */
	ULocalLightComponent* CreateLightComponent(const FSourceEntity& InEntity);

	/** Scales the configured brightness; 0 switches the light off outright. */
	void SetLightScale(float Scale);

private:
	lambda::IEntity* Behaviour = nullptr;
	lambda::EntityId GameId = lambda::InvalidEntity;

	UPROPERTY()
	TObjectPtr<ULocalLightComponent> LightComponent;

	/** What the light is worth at full brightness, before the game's pattern scales it. */
	float BaseIntensity = 0.0f;
};
