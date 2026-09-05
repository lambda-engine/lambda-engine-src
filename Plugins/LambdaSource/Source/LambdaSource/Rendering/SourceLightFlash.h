#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SourceLightFlash.generated.h"

class UPointLightComponent;

/**
 * A dynamic light that lives for a moment and fades: Source's dlight_t with a die time and a decay, which is
 * how an explosion lights the room it goes off in. It takes itself away when it has faded.
 */
UCLASS()
class LAMBDASOURCE_API ASourceLightFlash : public AActor
{
	GENERATED_BODY()

public:
	ASourceLightFlash();

	/**
	 * Lights up at Location: IntensityCandelas at full, reaching RadiusCm, gone after LifeSeconds. The light
	 * fades over its life and its reach shrinks with it, as a dlight's radius decays.
	 */
	static ASourceLightFlash* Create(UWorld* World, const FVector& Location, const FLinearColor& Color,
		float IntensityCandelas, float RadiusCm, float LifeSeconds, bool bCastShadows);

	virtual void Tick(float DeltaSeconds) override;

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UPointLightComponent> Light;

	float StartTime = 0.0f;
	float LifeSeconds = 0.3f;
	float PeakIntensity = 0.0f;
	float PeakRadius = 0.0f;
};
