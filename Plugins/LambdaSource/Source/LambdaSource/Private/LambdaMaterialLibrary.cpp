#include "LambdaMaterialLibrary.h"
#include "LambdaFileSystem.h"
#include "LambdaSourceModule.h"
#include "LambdaSourceSettings.h"
#include "SourceKeyValues.h"
#include "SourceVTFFile.h"
#include "SourceDecalScript.h"
#include "SourceDXTDecode.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "TextureResource.h"
#include "RenderUtils.h"

// ---------------------------------------------------------------------------------------------------------------------
// Name normalisation
// ---------------------------------------------------------------------------------------------------------------------

static FString StripPrefixAndExtension(const FString& InName, const TCHAR* Extension)
{
	FString Name = FLambdaFileSystem::NormalizeRelativePath(InName).ToLower();
	if (Name.StartsWith(TEXT("materials/")))
	{
		Name.RightChopInline(10);
	}
	if (Name.EndsWith(Extension))
	{
		Name.LeftChopInline(FCString::Strlen(Extension));
	}
	return Name;
}

FString ULambdaMaterialLibrary::NormalizeMaterialName(const FString& InName)
{
	return StripPrefixAndExtension(InName, TEXT(".vmt"));
}

FString ULambdaMaterialLibrary::NormalizeTextureName(const FString& InName)
{
	return StripPrefixAndExtension(InName, TEXT(".vtf"));
}

// ---------------------------------------------------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------------------------------------------------

void ULambdaMaterialLibrary::Initialize()
{
	if (bInitialized)
	{
		return;
	}
	bInitialized = true;

	const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();

	if (Settings.MasterMaterial.IsValid())
	{
		MasterMaterial = Cast<UMaterialInterface>(Settings.MasterMaterial.TryLoad());
	}
	if (Settings.FallbackMaterial.IsValid())
	{
		FallbackMaterial = Cast<UMaterialInterface>(Settings.FallbackMaterial.TryLoad());
	}
	if (Settings.DecalMaterial.IsValid())
	{
		DecalMasterMaterial = Cast<UMaterialInterface>(Settings.DecalMaterial.TryLoad());
	}
	if (Settings.DecalPBRMaterial.IsValid())
	{
		DecalPBRMasterMaterial = Cast<UMaterialInterface>(Settings.DecalPBRMaterial.TryLoad());
	}
	if (Settings.SpriteMaterial.IsValid())
	{
		SpriteMasterMaterial = Cast<UMaterialInterface>(Settings.SpriteMaterial.TryLoad());
	}
	if (Settings.SpriteMaterialNoZ.IsValid())
	{
		SpriteMasterMaterialNoZ = Cast<UMaterialInterface>(Settings.SpriteMaterialNoZ.TryLoad());
	}
	if (Settings.SpriteMaterialTranslucent.IsValid())
	{
		SpriteMasterMaterialTranslucent = Cast<UMaterialInterface>(Settings.SpriteMaterialTranslucent.TryLoad());
	}
	if (Settings.ModelMaterial.IsValid())
	{
		ModelMasterMaterial = Cast<UMaterialInterface>(Settings.ModelMaterial.TryLoad());
	}
	if (Settings.ModelMaterialTranslucent.IsValid())
	{
		ModelMasterMaterialTranslucent = Cast<UMaterialInterface>(Settings.ModelMaterialTranslucent.TryLoad());
	}
	if (Settings.ModelMaterialMasked.IsValid())
	{
		ModelMasterMaterialMasked = Cast<UMaterialInterface>(Settings.ModelMaterialMasked.TryLoad());
	}
	if (!FallbackMaterial)
	{
		FallbackMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
	}
	if (!MasterMaterial)
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Master material '%s' not found - world surfaces will use the fallback material (run Tools/CreateAssets.bat)"),
			*Settings.MasterMaterial.ToString());
	}
	else
	{
		UE_LOG(LogLambdaSource, Log, TEXT("Master material: %s"), *MasterMaterial->GetPathName());
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// VMT parsing
// ---------------------------------------------------------------------------------------------------------------------

void ULambdaMaterialLibrary::ApplyPatchBlock(const FSourceKeyValues* Block, FSourceKeyValues& Target, bool bInsertOnly)
{
	if (!Block)
	{
		return;
	}
	for (const FSourceKeyValues& Entry : Block->Children)
	{
		if (FSourceKeyValues* Existing = Target.FindChild(Entry.Key))
		{
			if (!bInsertOnly)
			{
				*Existing = Entry;
			}
		}
		else
		{
			Target.Children.Add(Entry);
		}
	}
}

bool ULambdaMaterialLibrary::LoadMaterialInfo(const FString& SourceMaterialName, FSourceMaterialInfo& OutInfo, FString* OutError, int32 Depth)
{
	const FString Name = NormalizeMaterialName(SourceMaterialName);
	const FString RelPath = FString::Printf(TEXT("materials/%s.vmt"), *Name);

	TArray<uint8> Bytes;
	if (!FLambdaFileSystem::Get().ReadFile(RelPath, Bytes))
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("VMT not found: %s"), *RelPath);
		}
		return false;
	}

	FSourceKeyValues Root;
	FString ParseError;
	if (!FSourceKeyValues::ParseSingle(Bytes, Root, &ParseError))
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("VMT parse error in %s: %s"), *RelPath, *ParseError);
		}
		return false;
	}

	// "patch" materials wrap another VMT and insert/replace parameters.
	if (Root.Key.Equals(TEXT("patch"), ESearchCase::IgnoreCase))
	{
		if (Depth > 4)
		{
			if (OutError)
			{
				*OutError = FString::Printf(TEXT("VMT patch chain too deep at %s"), *RelPath);
			}
			return false;
		}
		const FString Include = Root.GetString(TEXT("include"));
		FSourceMaterialInfo BaseInfo;
		if (Include.IsEmpty() || !LoadMaterialInfo(Include, BaseInfo, OutError, Depth + 1))
		{
			return false;
		}
		// Re-read the base VMT so replace/insert can be applied on its parameters.
		const FString BaseRel = FString::Printf(TEXT("materials/%s.vmt"), *NormalizeMaterialName(Include));
		TArray<uint8> BaseBytes;
		FSourceKeyValues BaseRoot;
		if (FLambdaFileSystem::Get().ReadFile(BaseRel, BaseBytes) && FSourceKeyValues::ParseSingle(BaseBytes, BaseRoot))
		{
			ApplyPatchBlock(Root.FindChild(TEXT("replace")), BaseRoot, false);
			ApplyPatchBlock(Root.FindChild(TEXT("insert")), BaseRoot, true);
			Root = MoveTemp(BaseRoot);
		}
		OutInfo.bIsPatch = true;
	}

	OutInfo.Name = Name;
	OutInfo.Shader = Root.Key;
	OutInfo.BaseTexture = NormalizeTextureName(Root.GetString(TEXT("$basetexture")));
	OutInfo.BaseTexture2 = NormalizeTextureName(Root.GetString(TEXT("$basetexture2")));
	OutInfo.bTranslucent = Root.GetBool(TEXT("$translucent"));
	OutInfo.bAlphaTest = Root.GetBool(TEXT("$alphatest"));
	OutInfo.bNoCull = Root.GetBool(TEXT("$nocull"));
	OutInfo.bSelfIllum = Root.GetBool(TEXT("$selfillum"));
	OutInfo.SurfaceProp = Root.GetString(TEXT("$surfaceprop"));
	OutInfo.bIgnoreZ = Root.GetBool(TEXT("$ignorez"));
	OutInfo.DecalScale = Root.GetFloat(TEXT("$decalscale"), 1.0f);
	OutInfo.bAdditive = Root.GetBool(TEXT("$additive"));
	OutInfo.Roughness = Root.GetFloat(TEXT("$roughness"), -1.0f);
	OutInfo.Metalness = Root.GetFloat(TEXT("$metalness"), -1.0f);
	OutInfo.bNormalMapFlipY = Root.GetBool(TEXT("$normalmapflipy"));
	OutInfo.bPhong = Root.GetBool(TEXT("$phong"));
	OutInfo.PhongExponent = Root.GetFloat(TEXT("$phongexponent"), 5.0f);
	OutInfo.SelfIllumMask = NormalizeTextureName(Root.GetString(TEXT("$selfillummask")));
	{
		// "[r g b]" is 0..1, "{r g b}" is 0..255 (materialsystem's two colour spellings).
		auto ParseColor = [](const FString& Text, FVector3f& Out)
		{
			if (Text.IsEmpty()) { return; }
			const bool bBytes = Text.Contains(TEXT("{"));
			FString Clean = Text.Replace(TEXT("["), TEXT(" ")).Replace(TEXT("]"), TEXT(" ")).Replace(TEXT("{"), TEXT(" ")).Replace(TEXT("}"), TEXT(" "));
			TArray<FString> Parts;
			Clean.ParseIntoArrayWS(Parts);
			if (Parts.Num() >= 3)
			{
				Out = FVector3f(FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]), FCString::Atof(*Parts[2]));
				if (bBytes) { Out /= 255.0f; }
			}
			else if (Parts.Num() == 1)
			{
				const float V = FCString::Atof(*Parts[0]);
				Out = FVector3f(V, V, V);
			}
		};
		ParseColor(Root.GetString(TEXT("$selfillumtint")), OutInfo.SelfIllumTint);
		ParseColor(Root.GetString(TEXT("$color2")), OutInfo.Color2);
	}
	OutInfo.BumpMap = NormalizeTextureName(Root.GetString(TEXT("$bumpmap")));
	OutInfo.HeightMap = NormalizeTextureName(Root.GetString(TEXT("$heightmap")));
	OutInfo.AOTexture = NormalizeTextureName(Root.GetString(TEXT("$aotexture")));
	OutInfo.HeightMapScale = Root.GetFloat(TEXT("$heightmapscale"), 0.1f);
	OutInfo.DecalSizeUnits = Root.GetFloat(TEXT("$decalsize"), 0.0f);
	OutInfo.DecalSizeVariance = Root.GetFloat(TEXT("$decalsizevariance"), 0.0f);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------------------------------------------------

UMaterialInterface* ULambdaMaterialLibrary::GetMaterial(const FString& SourceMaterialName)
{
	Initialize();
	const FString Name = NormalizeMaterialName(SourceMaterialName);
	if (TObjectPtr<UMaterialInterface>* Found = MaterialCache.Find(Name))
	{
		return *Found ? Found->Get() : FallbackMaterial.Get();
	}
	UMaterialInterface* Material = CreateMaterial(Name);
	MaterialCache.Add(Name, Material);
	return Material ? Material : FallbackMaterial.Get();
}

UMaterialInterface* ULambdaMaterialLibrary::CreateMaterial(const FString& Name)
{
	FSourceMaterialInfo Info;
	FString Error;
	if (!LoadMaterialInfo(Name, Info, &Error))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Material '%s': %s"), *Name, *Error);
		return nullptr;
	}

	UTexture2D* BaseTexture = Info.BaseTexture.IsEmpty() ? nullptr : GetTexture(Info.BaseTexture);
	if (!BaseTexture && !Info.BaseTexture2.IsEmpty())
	{
		BaseTexture = GetTexture(Info.BaseTexture2);
	}

	if (!MasterMaterial)
	{
		return nullptr;
	}

	// A material that brings a normal map, surface values, a tint or self-illumination goes through the lit PBR
	// master; "$translucent 1" through its alpha-blended twin; everything else stays on the plain base master.
	const bool bWantsPBR = !Info.BumpMap.IsEmpty() || Info.Roughness >= 0.0f || Info.Metalness >= 0.0f || Info.bSelfIllum
		|| Info.bPhong || !Info.Color2.Equals(FVector3f(1, 1, 1));
	UMaterialInterface* Master = MasterMaterial.Get();
	if (Info.bTranslucent && ModelMasterMaterialTranslucent)
	{
		Master = ModelMasterMaterialTranslucent.Get();
	}
	else if (Info.bAlphaTest && ModelMasterMaterialMasked)
	{
		// "$alphatest 1": the base alpha is a cut-out, not glass - an open crate's lattice is holes.
		Master = ModelMasterMaterialMasked.Get();
	}
	else if (bWantsPBR && ModelMasterMaterial)
	{
		Master = ModelMasterMaterial.Get();
	}

	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Master, this, *FString::Printf(TEXT("MID_%s"), *Name.Replace(TEXT("/"), TEXT("_"))));
	if (!MID)
	{
		return nullptr;
	}
	if (BaseTexture)
	{
		MID->SetTextureParameterValue(ULambdaSourceSettings::Get().BaseTextureParameterName, BaseTexture);
	}
	else
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Material '%s' (shader %s) has no usable $basetexture ('%s')"), *Name, *Info.Shader, *Info.BaseTexture);
	}
	if (Master != MasterMaterial.Get())
	{
		if (!Info.BumpMap.IsEmpty())
		{
			if (UTexture2D* Normal = GetTexture(Info.BumpMap, /*bSRGB=*/ false))
			{
				MID->SetTextureParameterValue(TEXT("NormalMap"), Normal);
			}
		}
		MID->SetScalarParameterValue(TEXT("FlipGreen"), Info.bNormalMapFlipY ? 1.0f : 0.0f);
		if (Info.Roughness >= 0.0f)
		{
			MID->SetScalarParameterValue(TEXT("Roughness"), Info.Roughness);
		}
		else if (Info.bPhong)
		{
			// Blinn-Phong exponent -> microfacet roughness, the usual sqrt(2 / (n + 2)) fit: $phongexponent 25 is
			// a 0.27 roughness, 5 (Source's default) 0.53. $phongexponenttexture (per-pixel) is not carried over.
			MID->SetScalarParameterValue(TEXT("Roughness"), FMath::Clamp(FMath::Sqrt(2.0f / (FMath::Max(Info.PhongExponent, 0.0f) + 2.0f)), 0.05f, 1.0f));
		}
		if (Info.Metalness >= 0.0f) { MID->SetScalarParameterValue(TEXT("Metalness"), Info.Metalness); }
		MID->SetVectorParameterValue(TEXT("ColorTint"), FLinearColor(Info.Color2.X, Info.Color2.Y, Info.Color2.Z, 1.0f));
		if (Info.bSelfIllum)
		{
			// $selfillum masks with the base texture's alpha (the pistol's sight dots) unless $selfillummask names a
			// texture; $selfillumtint defaults to white.
			UTexture2D* Mask = Info.SelfIllumMask.IsEmpty() ? nullptr : GetTexture(Info.SelfIllumMask, false);
			if (Mask)
			{
				MID->SetTextureParameterValue(TEXT("SelfIllumMask"), Mask);
			}
			MID->SetScalarParameterValue(TEXT("SelfIllumFromBaseAlpha"), Mask ? 0.0f : 1.0f);
			MID->SetVectorParameterValue(TEXT("SelfIllumTint"), FLinearColor(Info.SelfIllumTint.X, Info.SelfIllumTint.Y, Info.SelfIllumTint.Z, 1.0f));
		}
	}
	UE_LOG(LogLambdaSource, Verbose, TEXT("Material '%s': shader=%s basetexture=%s%s"), *Name, *Info.Shader, *Info.BaseTexture, Info.bIsPatch ? TEXT(" (patch)") : TEXT(""));
	return MID;
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------------------------------------------------

UTexture2D* ULambdaMaterialLibrary::GetTexture(const FString& SourceTextureName, bool bSRGB)
{
	const FString Name = NormalizeTextureName(SourceTextureName);
	if (Name.IsEmpty())
	{
		return nullptr;
	}
	// The same VTF can legitimately be wanted both ways, so the encoding is part of the cache key.
	const FString Key = bSRGB ? Name : Name + TEXT("#raw");
	if (TObjectPtr<UTexture2D>* Found = TextureCache.Find(Key))
	{
		return Found->Get();
	}
	UTexture2D* Texture = CreateTexture(Name, bSRGB);
	TextureCache.Add(Key, Texture);
	return Texture;
}

UTexture2D* ULambdaMaterialLibrary::CreateTexture(const FString& Name, bool bSRGB)
{
	const FString RelPath = FString::Printf(TEXT("materials/%s.vtf"), *Name);
	TArray<uint8> Bytes;
	if (!FLambdaFileSystem::Get().ReadFile(RelPath, Bytes))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Texture '%s': VTF not found (%s)"), *Name, *RelPath);
		return nullptr;
	}

	FSourceVTFFile VTF;
	FString Error;
	if (!VTF.Load(MoveTemp(Bytes), &Error))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Texture '%s': %s"), *Name, *Error);
		return nullptr;
	}

	UTexture2D* Texture = CreateTextureFromVTF(VTF, Name, &Error, bSRGB);
	if (!Texture)
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Texture '%s': %s"), *Name, *Error);
		return nullptr;
	}
	UE_LOG(LogLambdaSource, Verbose, TEXT("Texture '%s': %dx%d %s mips=%d flags=0x%08x (VTF %d.%d)"), *Name, VTF.GetWidth(), VTF.GetHeight(),
		FSourceVTFFile::GetFormatName(VTF.GetFormat()), VTF.GetNumMips(), VTF.GetFlags(), VTF.GetHeader().VersionMajor, VTF.GetHeader().VersionMinor);
	return Texture;
}

namespace
{
	/** Expands/swizzles an uncompressed VTF pixel row into BGRA8. Returns false for unsupported formats. */
	bool ConvertPixelsToBGRA8(ESourceImageFormat Format, const uint8* Src, int32 NumPixels, uint8* Dst)
	{
		switch (Format)
		{
		case ESourceImageFormat::BGRA8888:
			FMemory::Memcpy(Dst, Src, (SIZE_T)NumPixels * 4);
			return true;
		case ESourceImageFormat::BGRX8888:
			for (int32 i = 0; i < NumPixels; ++i) { Dst[0] = Src[0]; Dst[1] = Src[1]; Dst[2] = Src[2]; Dst[3] = 255; Src += 4; Dst += 4; }
			return true;
		case ESourceImageFormat::RGBA8888:
		case ESourceImageFormat::RGBX8888:
			for (int32 i = 0; i < NumPixels; ++i) { Dst[0] = Src[2]; Dst[1] = Src[1]; Dst[2] = Src[0]; Dst[3] = (Format == ESourceImageFormat::RGBA8888) ? Src[3] : 255; Src += 4; Dst += 4; }
			return true;
		case ESourceImageFormat::ABGR8888:
			for (int32 i = 0; i < NumPixels; ++i) { Dst[0] = Src[1]; Dst[1] = Src[2]; Dst[2] = Src[3]; Dst[3] = Src[0]; Src += 4; Dst += 4; }
			return true;
		case ESourceImageFormat::ARGB8888:
			for (int32 i = 0; i < NumPixels; ++i) { Dst[0] = Src[3]; Dst[1] = Src[2]; Dst[2] = Src[1]; Dst[3] = Src[0]; Src += 4; Dst += 4; }
			return true;
		case ESourceImageFormat::RGB888:
		case ESourceImageFormat::RGB888_BLUESCREEN:
			for (int32 i = 0; i < NumPixels; ++i) { Dst[0] = Src[2]; Dst[1] = Src[1]; Dst[2] = Src[0]; Dst[3] = 255; Src += 3; Dst += 4; }
			return true;
		case ESourceImageFormat::BGR888:
		case ESourceImageFormat::BGR888_BLUESCREEN:
			for (int32 i = 0; i < NumPixels; ++i) { Dst[0] = Src[0]; Dst[1] = Src[1]; Dst[2] = Src[2]; Dst[3] = 255; Src += 3; Dst += 4; }
			return true;
		case ESourceImageFormat::I8:
			for (int32 i = 0; i < NumPixels; ++i) { Dst[0] = Dst[1] = Dst[2] = Src[0]; Dst[3] = 255; Src += 1; Dst += 4; }
			return true;
		case ESourceImageFormat::IA88:
			for (int32 i = 0; i < NumPixels; ++i) { Dst[0] = Dst[1] = Dst[2] = Src[0]; Dst[3] = Src[1]; Src += 2; Dst += 4; }
			return true;
		case ESourceImageFormat::A8:
			for (int32 i = 0; i < NumPixels; ++i) { Dst[0] = Dst[1] = Dst[2] = 255; Dst[3] = Src[0]; Src += 1; Dst += 4; }
			return true;
		case ESourceImageFormat::RGB565:
			for (int32 i = 0; i < NumPixels; ++i)
			{
				const uint16 P = (uint16)(Src[0] | (Src[1] << 8));
				const uint8 R = (uint8)(((P >> 11) & 0x1F) * 255 / 31), G = (uint8)(((P >> 5) & 0x3F) * 255 / 63), B = (uint8)((P & 0x1F) * 255 / 31);
				Dst[0] = B; Dst[1] = G; Dst[2] = R; Dst[3] = 255; Src += 2; Dst += 4;
			}
			return true;
		case ESourceImageFormat::BGR565:
			for (int32 i = 0; i < NumPixels; ++i)
			{
				const uint16 P = (uint16)(Src[0] | (Src[1] << 8));
				const uint8 B = (uint8)(((P >> 11) & 0x1F) * 255 / 31), G = (uint8)(((P >> 5) & 0x3F) * 255 / 63), R = (uint8)((P & 0x1F) * 255 / 31);
				Dst[0] = B; Dst[1] = G; Dst[2] = R; Dst[3] = 255; Src += 2; Dst += 4;
			}
			return true;
		default:
			return false;
		}
	}
}

UTexture2D* ULambdaMaterialLibrary::CreateTextureFromVTF(const FSourceVTFFile& VTF, const FString& DebugName, FString* OutError, bool bSRGB)
{
	auto Fail = [&](const FString& Msg)
	{
		if (OutError)
		{
			*OutError = Msg;
		}
		return (UTexture2D*)nullptr;
	};

	if (!VTF.IsLoaded())
	{
		return Fail(TEXT("VTF not loaded"));
	}

	const ESourceImageFormat SrcFormat = VTF.GetFormat();
	const int32 Width = VTF.GetWidth();
	const int32 Height = VTF.GetHeight();
	const uint32 Flags = VTF.GetFlags();

	EPixelFormat PixelFormat = PF_Unknown;
	bool bConvertToBGRA = false;
	switch (SrcFormat)
	{
	case ESourceImageFormat::DXT1:
	case ESourceImageFormat::DXT1_ONEBITALPHA:
		PixelFormat = PF_DXT1;
		break;
	case ESourceImageFormat::DXT3:
		PixelFormat = PF_DXT3;
		break;
	case ESourceImageFormat::DXT5:
		PixelFormat = PF_DXT5;
		break;
	case ESourceImageFormat::ATI2N:
		PixelFormat = PF_BC5;
		break;
	case ESourceImageFormat::ATI1N:
		PixelFormat = PF_BC4;
		break;
	default:
		if (FSourceVTFFile::GetBytesPerPixel(SrcFormat) > 0)
		{
			PixelFormat = PF_B8G8R8A8;
			bConvertToBGRA = true;
		}
		break;
	}
	if (PixelFormat == PF_Unknown)
	{
		return Fail(FString::Printf(TEXT("Unsupported VTF image format %s"), FSourceVTFFile::GetFormatName(SrcFormat)));
	}

	const FPixelFormatInfo& FormatInfo = GPixelFormats[PixelFormat];
	if ((Width % FormatInfo.BlockSizeX) != 0 || (Height % FormatInfo.BlockSizeY) != 0)
	{
		return Fail(FString::Printf(TEXT("Texture size %dx%d is not a multiple of the %s block size"), Width, Height, FormatInfo.Name));
	}

	UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PixelFormat, NAME_None);
	if (!Texture)
	{
		return Fail(TEXT("UTexture2D::CreateTransient failed"));
	}

	const bool bIsNormalMap = (Flags & (FSourceVTFFile::FLAG_NORMAL | FSourceVTFFile::FLAG_SSBUMP)) != 0;
	Texture->SRGB = bSRGB && !bIsNormalMap;
	Texture->NeverStream = true;
	Texture->Filter = (Flags & FSourceVTFFile::FLAG_POINTSAMPLE) ? TF_Nearest : TF_Default;
	Texture->AddressX = (Flags & FSourceVTFFile::FLAG_CLAMPS) ? TA_Clamp : TA_Wrap;
	Texture->AddressY = (Flags & FSourceVTFFile::FLAG_CLAMPT) ? TA_Clamp : TA_Wrap;
	Texture->LODGroup = TEXTUREGROUP_World;
	Texture->CompressionSettings = bIsNormalMap ? TC_Normalmap : TC_Default;

	FTexturePlatformData* PlatformData = Texture->GetPlatformData();
	const int32 NumMips = (Flags & FSourceVTFFile::FLAG_NOMIP) ? 1 : VTF.GetNumMips();

	for (int32 MipLevel = 0; MipLevel < NumMips; ++MipLevel)
	{
		int32 MipW, MipH;
		VTF.GetMipDimensions(MipLevel, MipW, MipH);

		TConstArrayView<uint8> SrcData;
		if (!VTF.GetMipData(MipLevel, 0, 0, 0, SrcData))
		{
			return Fail(FString::Printf(TEXT("Could not read mip %d"), MipLevel));
		}

		const int64 BlocksX = FMath::Max(1, (MipW + FormatInfo.BlockSizeX - 1) / FormatInfo.BlockSizeX);
		const int64 BlocksY = FMath::Max(1, (MipH + FormatInfo.BlockSizeY - 1) / FormatInfo.BlockSizeY);
		const int64 DstBytes = BlocksX * BlocksY * FormatInfo.BlockBytes;

		FTexture2DMipMap* Mip = nullptr;
		if (MipLevel == 0)
		{
			Mip = &PlatformData->Mips[0];
		}
		else
		{
			Mip = new FTexture2DMipMap(MipW, MipH, 1);
			PlatformData->Mips.Add(Mip);
		}

		Mip->BulkData.Lock(LOCK_READ_WRITE);
		uint8* Dst = (uint8*)Mip->BulkData.Realloc(DstBytes);
		if (bConvertToBGRA)
		{
			if (!ConvertPixelsToBGRA8(SrcFormat, SrcData.GetData(), MipW * MipH, Dst))
			{
				Mip->BulkData.Unlock();
				return Fail(FString::Printf(TEXT("No converter for VTF format %s"), FSourceVTFFile::GetFormatName(SrcFormat)));
			}
		}
		else
		{
			if (SrcData.Num() != DstBytes)
			{
				Mip->BulkData.Unlock();
				return Fail(FString::Printf(TEXT("Mip %d size mismatch (VTF %d bytes, UE expects %lld)"), MipLevel, SrcData.Num(), DstBytes));
			}
			FMemory::Memcpy(Dst, SrcData.GetData(), DstBytes);
		}
		Mip->BulkData.Unlock();
	}

	Texture->UpdateResource();
	return Texture;
}

// ---------------------------------------------------------------------------------------------------------------------
// Surface properties, decals and sprites
// ---------------------------------------------------------------------------------------------------------------------

FString ULambdaMaterialLibrary::GetSurfaceProp(const FString& SourceMaterialName)
{
	Initialize();
	const FString Name = NormalizeMaterialName(SourceMaterialName);
	if (const FString* Cached = SurfacePropCache.Find(Name))
	{
		return *Cached;
	}

	FSourceMaterialInfo Info;
	FString SurfaceProp;
	if (LoadMaterialInfo(Name, Info))
	{
		SurfaceProp = Info.SurfaceProp;
	}
	SurfacePropCache.Add(Name, SurfaceProp);
	return SurfaceProp;
}

bool ULambdaMaterialLibrary::LoadDecalSubrect(const FString& NormalizedName, FSourceDecalSubrect& OutSubrect, FString& OutSheetTexture)
{
	// A "Subrect" VMT does not carry a texture: it names an atlas material plus the tile to cut from it.
	TArray<uint8> Bytes;
	if (!FLambdaFileSystem::Get().ReadFile(FString::Printf(TEXT("materials/%s.vmt"), *NormalizedName), Bytes))
	{
		return false;
	}
	FSourceKeyValues Root;
	if (!FSourceKeyValues::ParseSingle(Bytes, Root))
	{
		return false;
	}
	if (!Root.Key.Equals(TEXT("Subrect"), ESearchCase::IgnoreCase))
	{
		return false;
	}

	auto ParseVec2 = [](const FString& Text, FVector2D Default) -> FVector2D
	{
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(" "), true);
		return Parts.Num() >= 2 ? FVector2D(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1])) : Default;
	};

	OutSubrect.SheetMaterial = NormalizeMaterialName(Root.GetString(TEXT("$Material")));
	OutSubrect.Pos = ParseVec2(Root.GetString(TEXT("$Pos")), FVector2D::ZeroVector);
	OutSubrect.Size = ParseVec2(Root.GetString(TEXT("$Size")), FVector2D(64.0, 64.0));
	OutSubrect.DecalScale = Root.GetFloat(TEXT("$decalscale"), 0.1f);

	FSourceMaterialInfo SheetInfo;
	if (!LoadMaterialInfo(OutSubrect.SheetMaterial, SheetInfo))
	{
		return false;
	}
	OutSheetTexture = SheetInfo.BaseTexture;
	// DecalModulate is Source's mod2x blend; anything else (decals_lit) is an ordinary translucent decal.
	OutSubrect.bModulate = SheetInfo.Shader.Equals(TEXT("DecalModulate"), ESearchCase::IgnoreCase);
	return !OutSheetTexture.IsEmpty();
}

// Debug aids for the 3D decal: the height field drives both a parallax offset and a normal, and being able to
// zero either at runtime is how a bad-looking decal is traced to one or the other.
static float GDecalDepth = 1.0f;
static FAutoConsoleVariableRef CVarDecalDepth(
	TEXT("lambda.decal.depth"),
	GDecalDepth,
	TEXT("DecalDepth parameter on new impact decals (0 = flat Source-style decal)"));

static float GDecalNormalStrength = 3.0f;
static FAutoConsoleVariableRef CVarDecalNormalStrength(
	TEXT("lambda.decal.normalstrength"),
	GDecalNormalStrength,
	TEXT("NormalStrength parameter on new impact decals (0 = no normal perturbation)"));

// Authored (Source 2) decals: Source 2's g_flHeightMapScale is tuned for VR at arm's length, and the
// multiplier lets a flat screen at a few metres read the same depth; FlipGreen is for a normal map authored in
// the other handedness, which shades a crater as a bump.
static float GDecalHeightScaleMultiplier = 1.0f;
static FAutoConsoleVariableRef CVarDecalHeightScaleMultiplier(
	TEXT("lambda.decal.heightscale"),
	GDecalHeightScaleMultiplier,
	TEXT("Multiplier on an authored decal's $heightmapscale (its parallax depth)"));

static float GDecalFlipGreen = 0.0f;
static FAutoConsoleVariableRef CVarDecalFlipGreen(
	TEXT("lambda.decal.flipgreen"),
	GDecalFlipGreen,
	TEXT("1 = flip the green channel of authored decal normal maps"));

UMaterialInterface* ULambdaMaterialLibrary::GetDecalMaterial(const FString& SourceMaterialName, float& OutSizeUnits)
{
	Initialize();
	const FString Name = NormalizeMaterialName(SourceMaterialName);
	OutSizeUnits = 6.4f;	// 64 texels at the standard $decalscale 0.1

	if (TObjectPtr<UMaterialInterface>* Found = DecalCache.Find(Name))
	{
		if (const float* Size = DecalSizeCache.Find(Name))
		{
			OutSizeUnits = *Size;
		}
		return Found->Get();
	}

	UMaterialInterface* Result = nullptr;
	if (DecalMasterMaterial)
	{
		FSourceDecalSubrect Subrect;
		FString SheetTexture;
		FString TextureName;
		FVector4 UVRect(0.0, 0.0, 1.0, 1.0);	// (offsetU, offsetV, scaleU, scaleV)
		bool bModulate = false;			// DecalModulate (mod2x) rather than alpha-blended
		float PlainDecalScale = 0.0f;	// > 0: a plain decal, sized texture width * $decalscale (Source)

		if (LoadDecalSubrect(Name, Subrect, SheetTexture))
		{
			TextureName = SheetTexture;
			bModulate = Subrect.bModulate;
			OutSizeUnits = FMath::Max(Subrect.Size.X, Subrect.Size.Y) * Subrect.DecalScale;
		}
		else
		{
			// Not a subrect: an ordinary decal material with its own texture.
			FSourceMaterialInfo Info;
			if (LoadMaterialInfo(Name, Info))
			{
				TextureName = Info.BaseTexture;
				bModulate = Info.Shader.Equals(TEXT("DecalModulate"), ESearchCase::IgnoreCase);
				PlainDecalScale = Info.DecalScale;

				// A decal that brings its own normal, height or occlusion maps (Source 2 imports) is drawn by the
				// PBR master: authored crater data instead of depth derived from a scorch mark's brightness. A
				// blood splat has normal and occlusion but no height - it is simply flat (HeightScale 0).
				const bool bAuthored = !Info.HeightMap.IsEmpty() || !Info.BumpMap.IsEmpty() || !Info.AOTexture.IsEmpty();
				if (DecalPBRMasterMaterial && bAuthored)
				{
					UTexture2D* Color = TextureName.IsEmpty() ? nullptr : GetTexture(TextureName, true);
					UTexture2D* Normal = Info.BumpMap.IsEmpty() ? nullptr : GetTexture(Info.BumpMap, false);
					UTexture2D* Height = Info.HeightMap.IsEmpty() ? nullptr : GetTexture(Info.HeightMap, false);
					UTexture2D* AO = Info.AOTexture.IsEmpty() ? nullptr : GetTexture(Info.AOTexture, false);
					if (Color)
					{
						UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(DecalPBRMasterMaterial, this,
							*FString::Printf(TEXT("DecalPBR_%s"), *Name.Replace(TEXT("/"), TEXT("_"))));
						if (MID)
						{
							MID->SetTextureParameterValue(ULambdaSourceSettings::Get().BaseTextureParameterName, Color);
							if (Height) { MID->SetTextureParameterValue(TEXT("HeightMap"), Height); }
							if (Normal) { MID->SetTextureParameterValue(TEXT("NormalMap"), Normal); }
							if (AO) { MID->SetTextureParameterValue(TEXT("AOMap"), AO); }
							MID->SetScalarParameterValue(TEXT("HeightScale"), Height
								? Info.HeightMapScale * ULambdaSourceSettings::Get().DecalParallaxMultiplier * GDecalHeightScaleMultiplier
								: 0.0f);
							MID->SetScalarParameterValue(TEXT("DecalDepth"), Height ? GDecalDepth : 0.0f);
							MID->SetScalarParameterValue(TEXT("FlipGreen"), GDecalFlipGreen);
							if (Info.DecalSizeUnits > 0.0f)
							{
								OutSizeUnits = Info.DecalSizeUnits;
							}
							else if (Color->GetSizeX() > 0)
							{
								OutSizeUnits = Color->GetSizeX() * Info.DecalScale;
							}
							DecalCache.Add(Name, MID);
							DecalSizeCache.Add(Name, OutSizeUnits);
							DecalVarianceCache.Add(Name, FMath::Max(0.0f, Info.DecalSizeVariance));
							return MID;
						}
					}
				}
				if (Info.DecalSizeUnits > 0.0f)
				{
					OutSizeUnits = Info.DecalSizeUnits;
					PlainDecalScale = 0.0f;
				}
			}
		}

		// decalmodulate_dx9.cpp disables sRGB read and write - "keep everything in gamma space" - because mod2x
		// is a gamma-space blend where raw 128 is neutral. Read as sRGB, that neutral grey decodes to 0.216 and
		// the whole tile darkens the wall; the material converts the gamma factor to linear itself.
		if (UTexture2D* Texture = TextureName.IsEmpty() ? nullptr : GetTexture(TextureName, !bModulate))
		{
			if (Subrect.Size.X > 0.0 && Texture->GetSizeX() > 0 && Texture->GetSizeY() > 0 && !SheetTexture.IsEmpty())
			{
				const double SheetW = Texture->GetSizeX();
				const double SheetH = Texture->GetSizeY();
				UVRect = FVector4(Subrect.Pos.X / SheetW, Subrect.Pos.Y / SheetH,
					Subrect.Size.X / SheetW, Subrect.Size.Y / SheetH);
			}
			if (PlainDecalScale > 0.0f && Texture->GetSizeX() > 0)
			{
				// Source: a decal is its texture's mapping width times $decalscale, in world units.
				OutSizeUnits = Texture->GetSizeX() * PlainDecalScale;
			}

			// The parallax and the normal come from a height tile cut from this decal's part of the atlas, so
			// every decal in the group - concrete, metal, wood - gets its own shape, not one generic crater.
			// A plain (non-atlas) decal has no height tile and stays flat.
			UTexture2D* HeightMap = SheetTexture.IsEmpty() ? nullptr
				: CreateDecalHeightTexture(Name, SheetTexture, Subrect, Subrect.bModulate);

			UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(DecalMasterMaterial, this,
				*FString::Printf(TEXT("Decal_%s"), *Name.Replace(TEXT("/"), TEXT("_"))));
			if (MID)
			{
				MID->SetTextureParameterValue(ULambdaSourceSettings::Get().BaseTextureParameterName, Texture);
				if (HeightMap)
				{
					MID->SetTextureParameterValue(TEXT("HeightMap"), HeightMap);
				}
				MID->SetVectorParameterValue(TEXT("UVRect"), FLinearColor(UVRect.X, UVRect.Y, UVRect.Z, UVRect.W));
				// DecalModulate atlases carry their shape as brightness; translucent ones carry it in alpha.
				MID->SetScalarParameterValue(TEXT("Modulate"), bModulate ? 1.0f : 0.0f);
				MID->SetScalarParameterValue(TEXT("DecalDepth"), HeightMap ? GDecalDepth : 0.0f);
				MID->SetScalarParameterValue(TEXT("NormalStrength"), HeightMap ? GDecalNormalStrength : 0.0f);
				Result = MID;
			}
		}
	}

	if (!Result)
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Decal material '%s' could not be built (run Tools/CreateAssets.bat if M_LambdaDecal is missing)"), *Name);
	}
	DecalCache.Add(Name, Result);
	DecalSizeCache.Add(Name, OutSizeUnits);
	return Result;
}

UMaterialInterface* ULambdaMaterialLibrary::GetSpriteMaterial(const FString& SourceMaterialName)
{
	Initialize();
	const FString Name = NormalizeMaterialName(SourceMaterialName);
	if (TObjectPtr<UMaterialInterface>* Found = SpriteCache.Find(Name))
	{
		return Found->Get();
	}

	UMaterialInterface* Result = nullptr;
	if (SpriteMasterMaterial)
	{
		// Many effect sprites (effects/blood_gore, blood_drop, the dust and smoke puffs) are "Subrect" VMTs: a tile
		// of the particle/particle_composite atlas. The atlas material supplies shader flags and texture, the
		// subrect the UV rectangle.
		FSourceMaterialInfo Info;
		FSourceDecalSubrect Subrect;
		FString SheetTexture;
		bool bLoaded = false;
		bool bIsSubrect = false;
		if (LoadDecalSubrect(Name, Subrect, SheetTexture))
		{
			bIsSubrect = true;
			bLoaded = LoadMaterialInfo(Subrect.SheetMaterial, Info);
		}
		else
		{
			bLoaded = LoadMaterialInfo(Name, Info);
		}
		if (bLoaded)
		{
			// "$ignorez 1" sprites draw without a depth test - Source uses them for the first-person muzzle
			// flash so it is not occluded by the very view model it is attached to. "$additive" picks the additive
			// blend; a translucent sprite (blood, smoke, dust) alpha-blends and is tinted by vertex colour.
			UMaterialInterface* Master = SpriteMasterMaterial.Get();
			if (Info.bIgnoreZ && SpriteMasterMaterialNoZ)
			{
				Master = SpriteMasterMaterialNoZ.Get();
			}
			else if (!Info.bAdditive && SpriteMasterMaterialTranslucent)
			{
				Master = SpriteMasterMaterialTranslucent.Get();
			}
			if (UTexture2D* Texture = Info.BaseTexture.IsEmpty() ? nullptr : GetTexture(Info.BaseTexture))
			{
				UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Master, this,
					*FString::Printf(TEXT("Sprite_%s"), *Name.Replace(TEXT("/"), TEXT("_"))));
				if (MID)
				{
					MID->SetTextureParameterValue(ULambdaSourceSettings::Get().BaseTextureParameterName, Texture);
					if (bIsSubrect && Texture->GetSizeX() > 0 && Texture->GetSizeY() > 0)
					{
						const double SheetW = Texture->GetSizeX(), SheetH = Texture->GetSizeY();
						MID->SetVectorParameterValue(TEXT("UVRect"), FLinearColor(
							Subrect.Pos.X / SheetW, Subrect.Pos.Y / SheetH, Subrect.Size.X / SheetW, Subrect.Size.Y / SheetH));
					}
					Result = MID;
				}
			}
		}
	}

	if (!Result)
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Sprite material '%s' could not be built (master=%s)"),
			*Name, *GetNameSafe(SpriteMasterMaterial));
	}
	SpriteCache.Add(Name, Result);
	return Result;
}

void ULambdaMaterialLibrary::GetMaterialNames(TArray<FString>& OutNames) const
{
	MaterialCache.GetKeys(OutNames);
}

UTexture2D* ULambdaMaterialLibrary::CreateDecalHeightTexture(const FString& NormalizedName, const FString& SheetTexture,
	const FSourceDecalSubrect& Subrect, bool bModulate)
{
	if (TObjectPtr<UTexture2D>* Found = DecalHeightCache.Find(NormalizedName))
	{
		return Found->Get();
	}

	UTexture2D* Result = nullptr;

	TArray<uint8> Bytes;
	FSourceVTFFile VTF;
	TArray<FColor> Pixels;
	int32 SheetW = 0, SheetH = 0;
	if (FLambdaFileSystem::Get().ReadFile(FString::Printf(TEXT("materials/%s.vtf"), *SheetTexture), Bytes)
		&& VTF.Load(MoveTemp(Bytes))
		&& SourceDXT::DecodeToRGBA(VTF, 0, Pixels, SheetW, SheetH))
	{
		const int32 X0 = FMath::Clamp((int32)Subrect.Pos.X, 0, SheetW - 1);
		const int32 Y0 = FMath::Clamp((int32)Subrect.Pos.Y, 0, SheetH - 1);
		const int32 W = FMath::Clamp((int32)Subrect.Size.X, 1, SheetW - X0);
		const int32 H = FMath::Clamp((int32)Subrect.Size.Y, 1, SheetH - Y0);

		// Depth in 0..1: a mod2x tile's darkness (raw 128 is neutral, so depth = 1 - saturate(2 * luma)), or a
		// translucent tile's alpha.
		TArray<float> Depth;
		Depth.SetNumZeroed(W * H);
		for (int32 y = 0; y < H; ++y)
		{
			for (int32 x = 0; x < W; ++x)
			{
				const FColor& C = Pixels[(Y0 + y) * SheetW + (X0 + x)];
				float D;
				if (bModulate)
				{
					const float Luma = (0.30f * C.R + 0.59f * C.G + 0.11f * C.B) / 255.0f;
					D = 1.0f - FMath::Clamp(2.0f * Luma, 0.0f, 1.0f);
				}
				else
				{
					D = C.A / 255.0f;
				}
				Depth[y * W + x] = D;
			}
		}

		// Two passes of a 3x3 box blur: the atlas is a painted scorch mark, and as a height field its hard
		// texel edges would read as a pit with vertical walls rather than a crater with a rim.
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			TArray<float> Blurred;
			Blurred.SetNumZeroed(W * H);
			for (int32 y = 0; y < H; ++y)
			{
				for (int32 x = 0; x < W; ++x)
				{
					float Sum = 0.0f;
					for (int32 dy = -1; dy <= 1; ++dy)
					{
						for (int32 dx = -1; dx <= 1; ++dx)
						{
							const int32 SX = FMath::Clamp(x + dx, 0, W - 1);
							const int32 SY = FMath::Clamp(y + dy, 0, H - 1);
							Sum += Depth[SY * W + SX];
						}
					}
					Blurred[y * W + x] = Sum / 9.0f;
				}
			}
			Depth = MoveTemp(Blurred);
		}

		// Stored as height: white is the undisturbed surface, which is what the POM reference plane sits at.
		UTexture2D* Texture = UTexture2D::CreateTransient(W, H, PF_G8, NAME_None);
		if (Texture)
		{
			Texture->SRGB = false;
			Texture->NeverStream = true;
			Texture->Filter = TF_Trilinear;
			Texture->AddressX = TA_Clamp;
			Texture->AddressY = TA_Clamp;
			Texture->CompressionSettings = TC_Grayscale;

			// A full mip chain, so a hole seen from across the room minifies to a soft dot instead of aliasing
			// into a scratchy smudge. Each level is a 2x2 box average of the one above.
			TArray<uint8> Level;
			Level.SetNumUninitialized(W * H);
			for (int32 i = 0; i < W * H; ++i)
			{
				Level[i] = (uint8)FMath::RoundToInt((1.0f - FMath::Clamp(Depth[i], 0.0f, 1.0f)) * 255.0f);
			}

			FTexturePlatformData* PlatformData = Texture->GetPlatformData();
			int32 LevelW = W, LevelH = H;
			for (int32 MipLevel = 0; ; ++MipLevel)
			{
				FTexture2DMipMap* Mip;
				if (MipLevel == 0)
				{
					Mip = &PlatformData->Mips[0];
				}
				else
				{
					Mip = new FTexture2DMipMap(LevelW, LevelH);
					PlatformData->Mips.Add(Mip);
					Mip->BulkData.Lock(LOCK_READ_WRITE);
					Mip->BulkData.Realloc((int64)LevelW * LevelH);
					Mip->BulkData.Unlock();
				}
				uint8* Dst = (uint8*)Mip->BulkData.Lock(LOCK_READ_WRITE);
				FMemory::Memcpy(Dst, Level.GetData(), LevelW * LevelH);
				Mip->BulkData.Unlock();

				if (LevelW == 1 && LevelH == 1)
				{
					break;
				}
				const int32 NextW = FMath::Max(1, LevelW / 2);
				const int32 NextH = FMath::Max(1, LevelH / 2);
				TArray<uint8> Next;
				Next.SetNumUninitialized(NextW * NextH);
				for (int32 y = 0; y < NextH; ++y)
				{
					for (int32 x = 0; x < NextW; ++x)
					{
						const int32 SX0 = FMath::Min(x * 2, LevelW - 1), SX1 = FMath::Min(x * 2 + 1, LevelW - 1);
						const int32 SY0 = FMath::Min(y * 2, LevelH - 1), SY1 = FMath::Min(y * 2 + 1, LevelH - 1);
						const int32 Sum = Level[SY0 * LevelW + SX0] + Level[SY0 * LevelW + SX1]
							+ Level[SY1 * LevelW + SX0] + Level[SY1 * LevelW + SX1];
						Next[y * NextW + x] = (uint8)((Sum + 2) / 4);
					}
				}
				Level = MoveTemp(Next);
				LevelW = NextW;
				LevelH = NextH;
			}
			PlatformData->SetNumSlices(1);
			Texture->UpdateResource();
			Result = Texture;
		}
	}
	else
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Decal '%s': could not decode atlas '%s' for its height tile"), *NormalizedName, *SheetTexture);
	}

	DecalHeightCache.Add(NormalizedName, Result);
	return Result;
}

float ULambdaMaterialLibrary::GetDecalSizeVariance(const FString& SourceMaterialName) const
{
	const float* Found = DecalVarianceCache.Find(NormalizeMaterialName(SourceMaterialName));
	return Found ? *Found : 0.0f;
}
