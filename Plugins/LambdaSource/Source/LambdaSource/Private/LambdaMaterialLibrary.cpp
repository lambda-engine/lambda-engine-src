#include "LambdaMaterialLibrary.h"
#include "LambdaFileSystem.h"
#include "LambdaSourceModule.h"
#include "LambdaSourceSettings.h"
#include "SourceKeyValues.h"
#include "SourceVTFFile.h"
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

	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(MasterMaterial, this, *FString::Printf(TEXT("MID_%s"), *Name.Replace(TEXT("/"), TEXT("_"))));
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
	UE_LOG(LogLambdaSource, Verbose, TEXT("Material '%s': shader=%s basetexture=%s%s"), *Name, *Info.Shader, *Info.BaseTexture, Info.bIsPatch ? TEXT(" (patch)") : TEXT(""));
	return MID;
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------------------------------------------------

UTexture2D* ULambdaMaterialLibrary::GetTexture(const FString& SourceTextureName)
{
	const FString Name = NormalizeTextureName(SourceTextureName);
	if (Name.IsEmpty())
	{
		return nullptr;
	}
	if (TObjectPtr<UTexture2D>* Found = TextureCache.Find(Name))
	{
		return Found->Get();
	}
	UTexture2D* Texture = CreateTexture(Name);
	TextureCache.Add(Name, Texture);
	return Texture;
}

UTexture2D* ULambdaMaterialLibrary::CreateTexture(const FString& Name)
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

	UTexture2D* Texture = CreateTextureFromVTF(VTF, Name, &Error);
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

UTexture2D* ULambdaMaterialLibrary::CreateTextureFromVTF(const FSourceVTFFile& VTF, const FString& DebugName, FString* OutError)
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
	Texture->SRGB = !bIsNormalMap;
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
