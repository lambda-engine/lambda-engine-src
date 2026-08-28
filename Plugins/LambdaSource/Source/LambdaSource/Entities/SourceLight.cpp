#include "Entities/SourceLight.h"

#include "Core/LambdaSourceModule.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"

namespace SourceLightStyles
{
	// game/server/world.cpp, g_DefaultLightstyles. Installed there for indices 0-12 at map load, which is why
	// style 0 has a string at all.
	static const TCHAR* GDefaultLightstyles[] =
	{
		TEXT("m"),														// 0  normal
		TEXT("mmnmmommommnonmmonqnmmo"),									// 1  flicker A
		TEXT("abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba"),		// 2  slow, strong pulse
		TEXT("mmmmmaaaaammmmmaaaaaabcdefgabcdefg"),						// 3  candle A
		TEXT("mamamamamama"),											// 4  fast strobe
		TEXT("jklmnopqrstuvwxyzyxwvutsrqponmlkj"),						// 5  gentle pulse
		TEXT("nmonqnmomnmomomno"),										// 6  flicker B
		TEXT("mmmaaaabcdefgmmmmaaaammmaamm"),							// 7  candle B
		TEXT("mmmaaammmaaammmabcdefaaaammmmabcdefmmmaaaa"),				// 8  candle C
		TEXT("aaaaaaaazzzzzzzz"),										// 9  slow strobe
		TEXT("mmamammmmammamamaaamammma"),								// 10 fluorescent flicker
		TEXT("abcdefghijklmnopqrrqponmlkjihgfedcba"),					// 11 slow pulse, no black
		TEXT("mmnnmmnnnmmnn"),											// 12 underwater mutation
	};

	const TCHAR* GetDefaultLightstyleString(int32 StyleIndex)
	{
		return (StyleIndex >= 0 && StyleIndex < UE_ARRAY_COUNT(GDefaultLightstyles))
			? GDefaultLightstyles[StyleIndex] : TEXT("m");
	}

	int32 GetNumDefaultLightstyles()
	{
		return UE_ARRAY_COUNT(GDefaultLightstyles);
	}

	float BrightnessForChar(TCHAR Char)
	{
		// R_AnimateLight (engine/gl_rlight.cpp): k = (c - 'a') * 22, against a reference value of 256.
		const int32 Index = FMath::Clamp((int32)Char - (int32)TEXT('a'), 0, 25);
		return (float)(Index * 22) / 256.0f;
	}
}

namespace
{
	/** Ten characters a second, and it steps rather than blends - the strobes depend on that. */
	constexpr float GLightstyleFramesPerSecond = 10.0f;
	/** FadeThink's interval (lights.cpp: SetNextThink(curtime + 0.1f)). */
	constexpr float GFadeStepSeconds = 0.1f;
}

// Logs a line whenever a light's appearance steps to a new brightness. An animated light is hard to judge by
// eye - a strobe and a flicker look much the same at a glance, and a pattern that is subtly wrong looks like a
// pattern - so this prints what it is actually playing, character by character, to be read rather than watched.
static bool GLightDebug = false;
static FAutoConsoleVariableRef CVarLightDebug(
	TEXT("light_debug"),
	GLightDebug,
	TEXT("Log every light's appearance as it animates: name, pattern character and resulting brightness."));

ASourceLight::ASourceLight()
{
	PrimaryActorTick.bCanEverTick = true;
	// The pattern has to keep running while the player is elsewhere: a corridor whose strobe only starts once
	// you can see it reads as the light reacting to you.
	PrimaryActorTick.bStartWithTickEnabled = true;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

ULocalLightComponent* ASourceLight::CreateLightComponent(const FSourceEntity& InEntity)
{
	const bool bSpot = InEntity.ClassName.Equals(TEXT("light_spot"), ESearchCase::IgnoreCase);
	if (bSpot)
	{
		USpotLightComponent* Spot = NewObject<USpotLightComponent>(this, TEXT("SpotLight"));
		LightComponent = Spot;
	}
	else
	{
		UPointLightComponent* Point = NewObject<UPointLightComponent>(this, TEXT("PointLight"));
		LightComponent = Point;
	}
	LightComponent->SetupAttachment(GetRootComponent());
	LightComponent->RegisterComponent();
	return LightComponent;
}

void ASourceLight::InitializeLight(const FSourceEntity& InEntity, ASourceBSPWorldActor* InWorldActor)
{
	InitializeEntity(InEntity, InWorldActor);

	BaseIntensity = LightComponent ? LightComponent->Intensity : 0.0f;
	StyleIndex = Entity.GetInt(TEXT("style"), 0);

	// A custom appearance wins over the style dropdown - it is the more specific of the two, and Hammer offers
	// both on the same entity precisely so one can override the other.
	Pattern = Entity.Get(TEXT("pattern"));
	if (Pattern.IsEmpty())
	{
		// "defaultstyle" is what a light falls back to when it is switchable and has no pattern of its own.
		const int32 DefaultStyle = Entity.GetInt(TEXT("defaultstyle"), -1);
		if (StyleIndex < SourceLightStyles::GetNumDefaultLightstyles())
		{
			Pattern = SourceLightStyles::GetDefaultLightstyleString(StyleIndex);
		}
		else if (DefaultStyle > 0)
		{
			Pattern = SourceLightStyles::GetDefaultLightstyleString(DefaultStyle);
		}
		else
		{
			Pattern = TEXT("m");
		}
	}

	bIsOff = HasSpawnFlags(SourceLightFlags::SF_LIGHT_START_OFF);
	ApplyPattern(bIsOff ? TEXT("a") : Pattern);

	UE_LOG(LogLambdaSource, Verbose, TEXT("%s '%s': style %d, pattern '%s'%s"),
		*Entity.ClassName, *TargetName, StyleIndex, *Pattern, bIsOff ? TEXT(", initially dark") : TEXT(""));
}

void ASourceLight::ApplyPattern(const FString& NewActivePattern)
{
	ActivePattern = NewActivePattern.IsEmpty() ? TEXT("m") : NewActivePattern;
	bFading = false;
	// Applied immediately rather than waiting for the next tick, so a light switched off is dark in the same
	// frame the input arrived.
	LastAppliedChar = 0;
	Tick(0.0f);
}

void ASourceLight::ApplyBrightness(TCHAR Char)
{
	if (Char == LastAppliedChar || !LightComponent)
	{
		return;
	}
	LastAppliedChar = Char;

	const float Scale = SourceLightStyles::BrightnessForChar(Char);
	if (GLightDebug)
	{
		const FString What = bFading ? FString::Printf(TEXT("fading to '%s'"), *PendingPattern)
			: FString::Printf(TEXT("pattern='%s'"), *ActivePattern);
		UE_LOG(LogLambdaSource, Display, TEXT("light_debug t=%.2f '%s' style=%d %s char='%c' scale=%.3f intensity=%.1f"),
			GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f, *TargetName, StyleIndex, *What,
			(char)Char, Scale, BaseIntensity * Scale);
	}
	LightComponent->SetIntensity(BaseIntensity * Scale);
	// Total darkness is switched off rather than left burning at zero: a light with no output still costs a
	// shadow-casting pass every frame, and a corridor of flickering lights is mostly off at any moment.
	LightComponent->SetVisibility(Scale > 0.0f);
}

void ASourceLight::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!LightComponent)
	{
		return;
	}

	if (bFading)
	{
		// FadeThink: one character per tenth of a second towards the target, then the new pattern takes over.
		FadeTimer += DeltaSeconds;
		while (bFading && FadeTimer >= GFadeStepSeconds)
		{
			FadeTimer -= GFadeStepSeconds;
			if (CurrentFade < TargetFade)
			{
				++CurrentFade;
			}
			else if (CurrentFade > TargetFade)
			{
				--CurrentFade;
			}
			if (CurrentFade == TargetFade)
			{
				Pattern = PendingPattern;
				ApplyPattern(Pattern);
				return;
			}
		}
		ApplyBrightness(CurrentFade);
		return;
	}

	if (ActivePattern.IsEmpty())
	{
		return;
	}
	// R_AnimateLight: the frame is taken from the world clock, not accumulated per light, so every light on the
	// same pattern stays in step no matter when it was switched on.
	const double Time = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	const int32 Frame = (int32)(Time * GLightstyleFramesPerSecond) % ActivePattern.Len();
	ApplyBrightness(ActivePattern[Frame]);
}

void ASourceLight::TurnOn()
{
	bIsOff = false;
	ApplyPattern(Pattern);
}

void ASourceLight::TurnOff()
{
	bIsOff = true;
	ApplyPattern(TEXT("a"));
}

void ASourceLight::ToggleLight()
{
	if (bIsOff)
	{
		TurnOn();
	}
	else
	{
		TurnOff();
	}
}

void ASourceLight::SetPattern(const FString& NewPattern)
{
	Pattern = NewPattern;
	// A light given a pattern is on, whatever it was before (InputSetPattern clears SF_LIGHT_START_OFF).
	bIsOff = false;
	ApplyPattern(Pattern);
}

void ASourceLight::FadeToPattern(const FString& NewPattern)
{
	if (NewPattern.IsEmpty())
	{
		return;
	}
	// From the first character of the pattern the light is *set to*, not the one it happens to be playing.
	// InputFadeToPattern reads m_iszPattern, which TurnOff leaves alone - so a light that was switched off
	// jumps back to its own first value and fades from there. Kept as Source has it.
	CurrentFade = Pattern.IsEmpty() ? TEXT('a') : Pattern[0];
	TargetFade = NewPattern[0];
	PendingPattern = NewPattern;
	FadeTimer = 0.0f;
	bIsOff = false;

	if (CurrentFade == TargetFade)
	{
		Pattern = PendingPattern;
		ApplyPattern(Pattern);
		return;
	}
	bFading = true;
	ApplyBrightness(CurrentFade);
}

bool ASourceLight::AcceptInput(const FString& InputName, AActor* Activator, AActor* Caller, const FString& Parameter)
{
	if (InputName.Equals(TEXT("TurnOn"), ESearchCase::IgnoreCase))
	{
		TurnOn();
		return true;
	}
	if (InputName.Equals(TEXT("TurnOff"), ESearchCase::IgnoreCase))
	{
		TurnOff();
		return true;
	}
	if (InputName.Equals(TEXT("Toggle"), ESearchCase::IgnoreCase))
	{
		ToggleLight();
		return true;
	}
	if (InputName.Equals(TEXT("SetPattern"), ESearchCase::IgnoreCase))
	{
		SetPattern(Parameter);
		return true;
	}
	if (InputName.Equals(TEXT("FadeToPattern"), ESearchCase::IgnoreCase))
	{
		FadeToPattern(Parameter);
		return true;
	}
	// CLight::Use with no use type: a light wired to something that just "uses" it toggles.
	if (InputName.Equals(TEXT("Use"), ESearchCase::IgnoreCase))
	{
		ToggleLight();
		return true;
	}
	return Super::AcceptInput(InputName, Activator, Caller, Parameter);
}
