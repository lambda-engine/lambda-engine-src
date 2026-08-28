#pragma once

#include "CoreMinimal.h"
#include "World/SourceEntity.h"
#include "SourceLight.generated.h"

class ULocalLightComponent;

/** lights.cpp spawnflags. */
namespace SourceLightFlags
{
	enum Type : int32
	{
		SF_LIGHT_START_OFF = 1,
	};
}

namespace SourceLightStyles
{
	/**
	 * The thirteen appearances a light can be given in Hammer, as Valve writes them (g_DefaultLightstyles,
	 * game/server/world.cpp). Style 0 is "normal" and is a pattern like any other - which is why an ordinary
	 * light is 'm' rather than simply unmodulated.
	 */
	LAMBDASOURCE_API const TCHAR* GetDefaultLightstyleString(int32 StyleIndex);
	LAMBDASOURCE_API int32 GetNumDefaultLightstyles();

	/**
	 * The brightness one character of a pattern asks for, as a multiplier.
	 *
	 * R_AnimateLight: 'a' is total darkness, 'm' is normal, 'z' is double bright. The engine works in
	 * (c - 'a') * 22 against a reference of 256, so 'm' comes out at 264/256 - a normal light is a hair
	 * brighter than an unstyled one, and that hair is Quake's, kept because a map lit against it expects it.
	 */
	LAMBDASOURCE_API float BrightnessForChar(TCHAR Char);
}

/**
 * A light placed in the map: light, light_spot.
 *
 * A port of CLight (game/server/lights.cpp) with one deliberate difference. In Source a light only answers to
 * TurnOn/TurnOff/Toggle when its style is 32 or above, because everything below that is baked into a lightmap
 * the game cannot rewrite; vrad hands named lights a switchable style so they can be moved at runtime. Nothing
 * here is baked - every light is a real dynamic light - so the inputs are honoured whatever the style, which is
 * what a mapper means when they wire one up. Copying the `>= 32` test would leave most lights silently ignoring
 * the inputs the FGD advertises.
 */
UCLASS()
class LAMBDASOURCE_API ASourceLight : public ASourceEntity
{
	GENERATED_BODY()

public:
	ASourceLight();

	/** Creates the light component this entity's classname calls for, before it is configured. */
	ULocalLightComponent* CreateLightComponent(const FSourceEntity& InEntity);

	/**
	 * Reads style, pattern and the initially-dark flag and applies the starting state.
	 * Runs after the component has been given its colour and intensity, which it takes as the pattern's full
	 * brightness.
	 */
	void InitializeLight(const FSourceEntity& InEntity, ASourceBSPWorldActor* InWorldActor);

	virtual void Tick(float DeltaSeconds) override;
	virtual bool AcceptInput(const FString& InputName, AActor* Activator, AActor* Caller, const FString& Parameter) override;

	void TurnOn();
	void TurnOff();
	void ToggleLight();
	/** InputSetPattern: adopt a pattern and switch on. */
	void SetPattern(const FString& NewPattern);
	/** InputFadeToPattern: walk one character at a time from the old pattern's first value to the new one's. */
	void FadeToPattern(const FString& NewPattern);

	UFUNCTION(BlueprintPure, Category = "Lambda")
	bool IsLightOn() const { return !bIsOff; }

	UFUNCTION(BlueprintPure, Category = "Lambda")
	ULocalLightComponent* GetLightComponent() const { return LightComponent; }

	/** The pattern currently driving the light - "a" while it is off. */
	const FString& GetActivePattern() const { return ActivePattern; }

private:
	UPROPERTY()
	TObjectPtr<ULocalLightComponent> LightComponent;

	/** What the light is worth at full brightness, before the pattern scales it. */
	float BaseIntensity = 0.0f;

	/** The appearance the light returns to when switched on: the custom pattern, or its style's. */
	FString Pattern;
	/** What is being played right now. Differs from Pattern while off, or mid-fade. */
	FString ActivePattern;
	int32 StyleIndex = 0;
	bool bIsOff = false;

	/** FadeThink's state: one character stepped towards another every tenth of a second. */
	bool bFading = false;
	TCHAR CurrentFade = 'a';
	TCHAR TargetFade = 'a';
	FString PendingPattern;
	float FadeTimer = 0.0f;

	/** The last character applied, so the component is only touched when the brightness actually changes. */
	TCHAR LastAppliedChar = 0;

	void ApplyPattern(const FString& NewActivePattern);
	void ApplyBrightness(TCHAR Char);
};
