#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UObject/SoftObjectPath.h"
#include "LambdaSourceSettings.generated.h"

/**
 * Project settings for the Source-format runtime (Project Settings > Plugins > Lambda Source).
 * Stored in DefaultGame.ini under [/Script/LambdaSource.LambdaSourceSettings].
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Lambda Source"))
class LAMBDASOURCE_API ULambdaSourceSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	ULambdaSourceSettings();

	static const ULambdaSourceSettings& Get() { return *GetDefault<ULambdaSourceSettings>(); }

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	// ---- Units ----

	/** Centimetres per Hammer unit. Valve's documented scale is 16 units = 1 foot (1.905). 2.54 makes 1 unit = 1 inch. */
	UPROPERTY(config, EditAnywhere, Category = "Units", meta = (ClampMin = "0.01", UIMin = "0.5", UIMax = "4.0"))
	float UnitScale = 1.905f;

	// ---- File system ----

	/**
	 * Game/mod directories searched for content (maps/, materials/, ...), in order. Relative paths are resolved against
	 * the project directory (src/). Can be overridden on the command line with -gamedir=<path>. Searched before VPKs.
	 */
	UPROPERTY(config, EditAnywhere, Category = "File System")
	TArray<FString> GameDirectories;

	/**
	 * VPK archives to mount, searched after GameDirectories (so loose mod files override packaged content). Point at the
	 * "_dir.vpk" of a multi-chunk set (e.g. .../hl2/hl2_textures_dir.vpk) or a single .vpk. Absolute or project-relative.
	 */
	UPROPERTY(config, EditAnywhere, Category = "File System")
	TArray<FString> MountedVPKs;

	// ---- Maps ----

	/** Map loaded at startup when none is given via -map=<name> or ?map=<name>. Resolved as maps/<name>.bsp. */
	UPROPERTY(config, EditAnywhere, Category = "Maps")
	FString DefaultMap = TEXT("test");

	// ---- Materials ----

	/** Master material used for world surfaces. Must expose a Texture2D parameter named "BaseTexture". */
	UPROPERTY(config, EditAnywhere, Category = "Materials", meta = (AllowedClasses = "/Script/Engine.MaterialInterface"))
	FSoftObjectPath MasterMaterial;

	/** Material used when the master material or a Source material cannot be loaded. */
	UPROPERTY(config, EditAnywhere, Category = "Materials", meta = (AllowedClasses = "/Script/Engine.MaterialInterface"))
	FSoftObjectPath FallbackMaterial;

	/** Name of the texture parameter on the master material. */
	UPROPERTY(config, EditAnywhere, Category = "Materials")
	FName BaseTextureParameterName = TEXT("BaseTexture");

	// ---- Lighting ----

	/** Multiplier from Source light brightness (the 4th component of _light, typically ~200) to UE candelas. */
	UPROPERTY(config, EditAnywhere, Category = "Lighting", meta = (ClampMin = "0.0"))
	float LightIntensityScale = 2.0f;

	/** Multiplier from light_environment brightness to UE directional light lux. */
	UPROPERTY(config, EditAnywhere, Category = "Lighting", meta = (ClampMin = "0.0"))
	float SunIntensityScale = 0.05f;

	/** Attenuation radius (Hammer units) used for point/spot lights that do not specify _zero_percent_distance. */
	UPROPERTY(config, EditAnywhere, Category = "Lighting", meta = (ClampMin = "1.0"))
	float DefaultLightRadiusUnits = 1536.0f;

	/** Whether BSP lights cast dynamic shadows. */
	UPROPERTY(config, EditAnywhere, Category = "Lighting")
	bool bLightsCastShadows = true;

	/** Intensity of the constant ambient sky light added to every map (0 disables it). Compensates for the missing lightmap bounce. */
	UPROPERTY(config, EditAnywhere, Category = "Lighting", meta = (ClampMin = "0.0"))
	float AmbientFillIntensity = 0.25f;

	/** Colour of the ambient fill light. */
	UPROPERTY(config, EditAnywhere, Category = "Lighting")
	FLinearColor AmbientFillColor = FLinearColor(0.6f, 0.65f, 0.75f);

	/** Exposure compensation (EV) applied through the player camera. */
	UPROPERTY(config, EditAnywhere, Category = "Lighting", meta = (UIMin = "-5.0", UIMax = "5.0"))
	float ExposureBias = 0.0f;

	// ---- Player (Hammer units; HL2 defaults) ----

	UPROPERTY(config, EditAnywhere, Category = "Player", meta = (ClampMin = "1.0"))
	float PlayerCapsuleRadiusUnits = 16.0f;

	UPROPERTY(config, EditAnywhere, Category = "Player", meta = (ClampMin = "1.0"))
	float PlayerCapsuleHalfHeightUnits = 36.0f;

	UPROPERTY(config, EditAnywhere, Category = "Player", meta = (ClampMin = "1.0"))
	float PlayerEyeHeightUnits = 64.0f;

	UPROPERTY(config, EditAnywhere, Category = "Player", meta = (ClampMin = "1.0"))
	float PlayerWalkSpeedUnits = 190.0f;

	UPROPERTY(config, EditAnywhere, Category = "Player", meta = (ClampMin = "1.0"))
	float PlayerSprintSpeedUnits = 320.0f;

	/** Jump height in units (HL2 GAMEMOVEMENT_JUMP_HEIGHT = 21). */
	UPROPERTY(config, EditAnywhere, Category = "Player", meta = (ClampMin = "0.0"))
	float PlayerJumpHeightUnits = 21.0f;

	/** sv_gravity */
	UPROPERTY(config, EditAnywhere, Category = "Player", meta = (ClampMin = "0.0"))
	float GravityUnits = 600.0f;

	/** sv_stepsize */
	UPROPERTY(config, EditAnywhere, Category = "Player", meta = (ClampMin = "0.0"))
	float StepHeightUnits = 18.0f;

	// ---- Helpers ----

	float UnitsToCm(float Units) const { return Units * UnitScale; }
};
