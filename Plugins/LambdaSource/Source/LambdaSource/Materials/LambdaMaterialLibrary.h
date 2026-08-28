#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "LambdaMaterialLibrary.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class UTexture2D;
class FSourceVTFFile;
struct FSourceKeyValues;

/** Minimal description of a parsed VMT. */
struct LAMBDASOURCE_API FSourceMaterialInfo
{
	FString Name;			// normalised material name, e.g. "dev/dev_measuregeneric01"
	FString Shader;			// e.g. "LightmappedGeneric"
	FString BaseTexture;	// normalised texture name (no extension), may be empty
	FString BaseTexture2;	// WorldVertexTransition second layer
	bool bTranslucent = false;
	bool bAlphaTest = false;
	bool bNoCull = false;
	bool bSelfIllum = false;
	bool bIsPatch = false;
	FString SurfaceProp;	// $surfaceprop: the key into scripts/surfaceproperties*.txt
	float DecalScale = 1.0f;	// $decalscale: decal world size = texture width * this (Source's decal sizing)
	bool bIgnoreZ = false;	// $ignorez: draw without depth testing (first-person effect sprites)
	bool bAdditive = false;	// $additive: additive blend (flashes, glows) rather than alpha
	// PBR inputs: Source 2 imports write real values (Lambda keys $roughness/$metalness); HL2 materials only have $bumpmap.
	float Roughness = -1.0f;	// $roughness, -1 = not given
	float Metalness = -1.0f;	// $metalness, -1 = not given
	bool bNormalMapFlipY = false;	// $normalmapflipy: the other tangent-space handedness
	FString SelfIllumMask;		// $selfillummask (texture name)
	FVector3f SelfIllumTint = FVector3f(1, 1, 1);	// $selfillumtint
	FVector3f Color2 = FVector3f(1, 1, 1);			// $color2 tint
	bool bPhong = false;		// $phong: Source's specular model; its exponent stands in for roughness
	float PhongExponent = 5.0f;	// $phongexponent (Source's default when no exponent texture)

	// Authored decal maps. $bumpmap is Source's own normal-map key; the rest are a Lambda extension written by
	// Tools/ImportSource2Decals.py for decals that come with height and occlusion maps (Source 2's bullet holes).
	FString BumpMap;			// $bumpmap: tangent-space normal
	FString HeightMap;			// $heightmap: single channel, white = undisturbed surface
	FString AOTexture;			// $aotexture: single channel occlusion
	float HeightMapScale = 0.1f;	// $heightmapscale: parallax depth as a fraction of the decal's width
	float DecalSizeUnits = 0.0f;	// $decalsize: world size in Hammer units (0 = not specified)
	float DecalSizeVariance = 0.0f;	// $decalsizevariance: random +/- on the size, in Hammer units (0 = none)
};

/**
 * Creates and caches UE materials/textures for Source material names (VMT + VTF) at runtime.
 * One instance per loaded map (owned by the BSP world actor) so everything is released with the map.
 */
UCLASS()
class LAMBDASOURCE_API ULambdaMaterialLibrary : public UObject
{
	GENERATED_BODY()

public:
	/** Loads the master/fallback materials from settings. Safe to call multiple times. */
	void Initialize();

	/** Returns a (cached) UE material for a Source material name ("dev/dev_measuregeneric01", "DEV/DEV_X", "materials/dev/x.vmt"...). Never null after Initialize. */
	UMaterialInterface* GetMaterial(const FString& SourceMaterialName);

	/**
	 * Returns a (cached) UE texture for a Source texture name ("dev/dev_measuregeneric01", "materials/dev/x.vtf"...).
	 * May be null. bSRGB is decided by the shader that will sample it, as in Source: lit and unlit base textures
	 * are gamma-encoded colour, but DecalModulate reads its atlas raw because mod2x is a gamma-space blend.
	 */
	UTexture2D* GetTexture(const FString& SourceTextureName, bool bSRGB = true);

	/** Parses a VMT (following "patch" includes). */
	bool LoadMaterialInfo(const FString& SourceMaterialName, FSourceMaterialInfo& OutInfo, FString* OutError = nullptr, int32 Depth = 0);

	/** $surfaceprop for a material name, "" when it declares none. Cached alongside the material. */
	FString GetSurfaceProp(const FString& SourceMaterialName);

	/**
	 * The size a world surface's texture coordinates are measured in - Source's IMaterial::GetMappingWidth/Height.
	 *
	 * A face's texture axes are stored in texels per world unit, so dividing by this is what turns them into the
	 * 0..1 range a sampler wants. It is the base texture's own size, read at load time, and deliberately not the
	 * width and height vbsp wrote into the BSP: those are the compiler's record of what it found on disk, and
	 * when it found nothing it records zero. Source never consults them for rendering either - see
	 * SurfComputeTextureCoordinate in engine/matsys_interface.cpp.
	 *
	 * Falls back to 128x128 for a material with no readable texture, which is the size of the dev textures a map
	 * missing its content ends up drawing anyway.
	 */
	FIntPoint GetMaterialMappingSize(const FString& SourceMaterialName);

	/**
	 * Builds a decal material for a Source decal name ("decals/concrete/shot3_subrect"), resolving the Subrect
	 * indirection every HL2 impact decal uses. OutSizeUnits is the decal's world size in Hammer units.
	 */
	UMaterialInterface* GetDecalMaterial(const FString& SourceMaterialName, float& OutSizeUnits);

	/** $decalsizevariance (random +/- in Hammer units) of a decal built with GetDecalMaterial; 0 when it has none. */
	float GetDecalSizeVariance(const FString& SourceMaterialName) const;

	/** Builds an additive unlit sprite material (muzzle flashes and other UnlitGeneric $additive effects). */
	UMaterialInterface* GetSpriteMaterial(const FString& SourceMaterialName);

	/** Names of every material created so far (normalised), for precaching what goes with them. */
	void GetMaterialNames(TArray<FString>& OutNames) const;

	UMaterialInterface* GetFallbackMaterial() const { return FallbackMaterial; }
	UMaterialInterface* GetMasterMaterial() const { return MasterMaterial; }
	int32 GetNumMaterials() const { return MaterialCache.Num(); }
	int32 GetNumTextures() const { return TextureCache.Num(); }

	/** Lower-case, forward slashes, no "materials/" prefix, no ".vmt". */
	static FString NormalizeMaterialName(const FString& InName);
	/** Lower-case, forward slashes, no "materials/" prefix, no ".vtf". */
	static FString NormalizeTextureName(const FString& InName);

	/** Builds a transient UTexture2D (with mip chain) from a parsed VTF. Returns null and sets OutError on failure. */
	static UTexture2D* CreateTextureFromVTF(const FSourceVTFFile& VTF, const FString& DebugName, FString* OutError = nullptr, bool bSRGB = true);

private:
	UMaterialInterface* CreateMaterial(const FString& NormalizedName);
	/** Reads a "Subrect" VMT and the atlas material it references. */
	bool LoadDecalSubrect(const FString& NormalizedName, struct FSourceDecalSubrect& OutSubrect, FString& OutSheetTexture);

	/**
	 * Builds the single-channel height tile a decal's parallax and normal are driven from: the decal's own tile
	 * of the atlas, decoded on the CPU, turned into depth (a DecalModulate tile's darkness, a translucent
	 * tile's alpha) and lightly smoothed. White is the undisturbed surface, dark is deep.
	 */
	UTexture2D* CreateDecalHeightTexture(const FString& NormalizedName, const FString& SheetTexture,
		const struct FSourceDecalSubrect& Subrect, bool bModulate);
	UTexture2D* CreateTexture(const FString& NormalizedName, bool bSRGB);
	static void ApplyPatchBlock(const FSourceKeyValues* Block, FSourceKeyValues& Target, bool bInsertOnly);

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> MasterMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> FallbackMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> DecalMasterMaterial;

	/** Decal master for decals with authored normal/height/AO maps (Source 2 imports). */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> DecalPBRMasterMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> SpriteMasterMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> SpriteMasterMaterialNoZ;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> SpriteMasterMaterialTranslucent;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> ModelMasterMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> ModelMasterMaterialTranslucent;
	TObjectPtr<UMaterialInterface> ModelMasterMaterialMasked;

	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UMaterialInterface>> DecalCache;

	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UMaterialInterface>> SpriteCache;

	/** Decal world size in Hammer units, keyed the same way as DecalCache. */
	TMap<FString, float> DecalSizeCache;

	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UTexture2D>> DecalHeightCache;

	TMap<FString, float> DecalVarianceCache;

	/** $surfaceprop per material name, so a bullet impact does not re-parse the VMT every shot. */
	TMap<FString, FString> SurfacePropCache;

	/** Base texture size by normalised material name; see GetMaterialMappingSize. */
	TMap<FString, FIntPoint> MappingSizeCache;

	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UMaterialInterface>> MaterialCache;

	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UTexture2D>> TextureCache;

	bool bInitialized = false;
};
