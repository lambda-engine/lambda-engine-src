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

	/** Deferred-decal master material used for bullet impacts. Needs a "BaseTexture" and a "UVRect" parameter. */
	UPROPERTY(config, EditAnywhere, Category = "Materials", meta = (AllowedClasses = "/Script/Engine.MaterialInterface"))
	FSoftObjectPath DecalMaterial;

	/** Decal master for decals with authored normal/height/AO maps (Tools/ImportSource2Decals.py output). */
	UPROPERTY(config, EditAnywhere, Category = "Materials", meta = (AllowedClasses = "/Script/Engine.MaterialInterface"))
	FSoftObjectPath DecalPBRMaterial;

	/**
	 * Multiplier on an authored decal's $heightmapscale. Half-Life: Alyx's g_flHeightMapScale (0.1) is tuned for VR
	 * at arm's length; on a flat screen a few metres away that is under a centimetre of parallax and a bullet hole
	 * reads as printed on. 3 makes the same decals read as holes; 1 is HL:A's own value.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Effects", meta = (ClampMin = "0.0"))
	float DecalParallaxMultiplier = 3.0f;

	/** Unlit additive master material used for muzzle flashes and other sprite effects. */
	UPROPERTY(config, EditAnywhere, Category = "Materials", meta = (AllowedClasses = "/Script/Engine.MaterialInterface"))
	FSoftObjectPath SpriteMaterial;

	/** The same, with the depth test off, for sprites whose VMT sets "$ignorez 1" (first-person muzzle flashes). */
	UPROPERTY(config, EditAnywhere, Category = "Materials", meta = (AllowedClasses = "/Script/Engine.MaterialInterface"))
	FSoftObjectPath SpriteMaterialNoZ;

	/** Alpha-blended unlit sprite master, vertex-coloured, for UnlitGeneric $translucent effects (blood, dust, smoke). */
	UPROPERTY(config, EditAnywhere, Category = "Materials", meta = (AllowedClasses = "/Script/Engine.MaterialInterface"))
	FSoftObjectPath SpriteMaterialTranslucent;

	// ---- Effects ----

	/** Seconds a bullet-impact decal stays before it fades out (Source's r_decal_cullsize/decal lifetime analogue). */
	UPROPERTY(config, EditAnywhere, Category = "Effects", meta = (ClampMin = "0.0"))
	float DecalLifetime = 30.0f;

	/** Whether a muzzle flash also spawns the dynamic light Source's ProcessMuzzleFlashEvent creates. */
	UPROPERTY(config, EditAnywhere, Category = "Effects")
	bool bMuzzleFlashLight = true;

	/** Brightness of that muzzle flash light, in candelas. */
	UPROPERTY(config, EditAnywhere, Category = "Effects", meta = (ClampMin = "0.0"))
	float MuzzleFlashLightIntensity = 2500.0f;

	/**
	 * Multiplier on the muzzle flash sprite's emissive. Source blends its flash additively over an LDR
	 * framebuffer with no exposure; UE renders HDR with auto-exposure, so the same texture values come out far
	 * dimmer - invisible against a lit wall. This is the compensation, not a Source value.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Effects", meta = (ClampMin = "0.0"))
	float MuzzleFlashBrightness = 6.0f;

	/**
	 * Reach of the muzzle flash light, in Hammer units. Source's elight is 32-64 units and only lights models,
	 * never the world - which is why a shot in HL2 lights your hands but not the room. A light that actually
	 * illuminates the room has to reach much further, so this is deliberately not Source's number.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Effects", meta = (ClampMin = "1.0"))
	float MuzzleFlashLightRadiusUnits = 300.0f;

	/**
	 * Whether the muzzle flash light also lights the view model. A light bright enough to reach across a room is
	 * centimetres from the player's hands, and inverse-square falloff blows them out, so this is off by default
	 * and the view model is excluded with a lighting channel.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Effects")
	bool bMuzzleFlashLightsViewModel = false;

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

	// ---- View model ----

	/**
	 * Offset of the first-person weapon model from the camera, in centimetres (forward, right, up). The model's
	 * own animation already places the hands and weapon relative to the eye, exactly as in Source, so this is a
	 * nudge on top of that - the equivalent of Source's viewmodel_offset_x/y/z - and zero is the faithful value.
	 */
	UPROPERTY(config, EditAnywhere, Category = "View Model")
	FVector ViewModelOffset = FVector::ZeroVector;

	/** Extra yaw/pitch/roll applied to the view model. Zero is the faithful value; see ViewModelOffset. */
	UPROPERTY(config, EditAnywhere, Category = "View Model")
	FRotator ViewModelRotation = FRotator(0.0f, 0.0f, 0.0f);

	/**
	 * Field of view the view model is drawn with, separate from the world's - Source's viewmodel_fov, which
	 * defaults to 54 in HL2 while the world runs at 90.
	 */
	UPROPERTY(config, EditAnywhere, Category = "View Model", meta = (ClampMin = "5.0", ClampMax = "170.0"))
	float ViewModelFOV = 54.0f;

	/**
	 * How far the view model is scaled towards the camera so it cannot intersect world geometry. This is UE's
	 * first-person rendering path, and it does the same job as Source's separate view model pass with its own
	 * compressed depth range: without it the gun pushes into a wall you stand against.
	 */
	UPROPERTY(config, EditAnywhere, Category = "View Model", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float ViewModelFirstPersonScale = 0.4f;

	/** Uniform scale on the view model. */
	UPROPERTY(config, EditAnywhere, Category = "View Model", meta = (ClampMin = "0.01"))
	float ViewModelScale = 1.0f;

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
