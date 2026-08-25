#pragma once

#include "CoreMinimal.h"

/** Image formats as stored in VTF files (public/bitmap/imageformat_declarations.h). */
enum class ESourceImageFormat : int32
{
	Unknown = -1,
	RGBA8888 = 0,
	ABGR8888,
	RGB888,
	BGR888,
	RGB565,
	I8,
	IA88,
	P8,
	A8,
	RGB888_BLUESCREEN,
	BGR888_BLUESCREEN,
	ARGB8888,
	BGRA8888,
	DXT1,
	DXT3,
	DXT5,
	BGRX8888,
	BGR565,
	BGRX5551,
	BGRA4444,
	DXT1_ONEBITALPHA,
	BGRA5551,
	UV88,
	UVWQ8888,
	RGBA16161616F,
	RGBA16161616,
	UVLX8888,
	R32F,
	RGB323232F,
	RGBA32323232F,
	RG1616F,
	RG3232F,
	RGBX8888,
	NullFormat,
	ATI2N,
	ATI1N,
	Count
};

/** Parsed VTF header (versions 7.0 - 7.5). */
struct FSourceVTFHeader
{
	int32 VersionMajor = 0;
	int32 VersionMinor = 0;
	uint32 HeaderSize = 0;
	int32 Width = 0;
	int32 Height = 0;
	uint32 Flags = 0;
	int32 NumFrames = 1;
	int32 StartFrame = 0;
	FVector3f Reflectivity = FVector3f::ZeroVector;
	float BumpScale = 1.0f;
	ESourceImageFormat ImageFormat = ESourceImageFormat::Unknown;
	int32 NumMipLevels = 1;
	ESourceImageFormat LowResImageFormat = ESourceImageFormat::Unknown;
	int32 LowResWidth = 0;
	int32 LowResHeight = 0;
	int32 Depth = 1;			// 7.2+
	int32 NumResources = 0;		// 7.3+
};

/**
 * Reader for Valve Texture Format files. Keeps the file in memory and exposes the mip chain (largest mip = level 0).
 */
class LAMBDASOURCE_API FSourceVTFFile
{
public:
	// TEXTUREFLAGS_* (public/vtf/vtf_declarations.h)
	static constexpr uint32 FLAG_POINTSAMPLE = 0x00000001;
	static constexpr uint32 FLAG_TRILINEAR = 0x00000002;
	static constexpr uint32 FLAG_CLAMPS = 0x00000004;
	static constexpr uint32 FLAG_CLAMPT = 0x00000008;
	static constexpr uint32 FLAG_ANISOTROPIC = 0x00000010;
	static constexpr uint32 FLAG_HINT_DXT5 = 0x00000020;
	static constexpr uint32 FLAG_PWL_CORRECTED = 0x00000040;
	static constexpr uint32 FLAG_NORMAL = 0x00000080;
	static constexpr uint32 FLAG_NOMIP = 0x00000100;
	static constexpr uint32 FLAG_NOLOD = 0x00000200;
	static constexpr uint32 FLAG_ALL_MIPS = 0x00000400;
	static constexpr uint32 FLAG_PROCEDURAL = 0x00000800;
	static constexpr uint32 FLAG_ONEBITALPHA = 0x00001000;
	static constexpr uint32 FLAG_EIGHTBITALPHA = 0x00002000;
	static constexpr uint32 FLAG_ENVMAP = 0x00004000;
	static constexpr uint32 FLAG_RENDERTARGET = 0x00008000;
	static constexpr uint32 FLAG_DEPTHRENDERTARGET = 0x00010000;
	static constexpr uint32 FLAG_NODEBUGOVERRIDE = 0x00020000;
	static constexpr uint32 FLAG_SINGLECOPY = 0x00040000;
	static constexpr uint32 FLAG_SRGB = 0x00080000;
	static constexpr uint32 FLAG_CLAMPU = 0x02000000;
	static constexpr uint32 FLAG_VERTEXTEXTURE = 0x04000000;
	static constexpr uint32 FLAG_SSBUMP = 0x08000000;
	static constexpr uint32 FLAG_BORDER = 0x20000000;

	/** Takes ownership of the file contents. */
	bool Load(TArray<uint8>&& InData, FString* OutError = nullptr);
	bool Load(const TArray<uint8>& InData, FString* OutError = nullptr);

	bool IsLoaded() const { return bLoaded; }
	const FSourceVTFHeader& GetHeader() const { return Header; }
	int32 GetWidth() const { return Header.Width; }
	int32 GetHeight() const { return Header.Height; }
	int32 GetNumMips() const { return Header.NumMipLevels; }
	int32 GetNumFrames() const { return Header.NumFrames; }
	int32 GetNumFaces() const { return NumFaces; }
	int32 GetDepth() const { return Header.Depth; }
	uint32 GetFlags() const { return Header.Flags; }
	ESourceImageFormat GetFormat() const { return Header.ImageFormat; }
	bool IsCubemap() const { return NumFaces > 1; }

	/** Dimensions of a mip level (0 = largest). */
	void GetMipDimensions(int32 MipLevel, int32& OutWidth, int32& OutHeight) const;
	/** Returns a view on the raw image data of one 2D image (MipLevel 0 = largest). */
	bool GetMipData(int32 MipLevel, int32 Frame, int32 Face, int32 Slice, TConstArrayView<uint8>& OutData) const;

	// ---- Format helpers ----
	static int64 ComputeImageBytes(int32 Width, int32 Height, ESourceImageFormat Format);
	static bool IsCompressedFormat(ESourceImageFormat Format);
	/** Bytes per pixel for uncompressed formats, 0 for compressed/unknown. */
	static int32 GetBytesPerPixel(ESourceImageFormat Format);
	/** Bytes per 4x4 block for compressed formats, 0 otherwise. */
	static int32 GetBlockBytes(ESourceImageFormat Format);
	static const TCHAR* GetFormatName(ESourceImageFormat Format);

private:
	int64 ComputeMipOffset(int32 MipLevel, int32 Frame, int32 Face, int32 Slice) const;

	TArray<uint8> Data;
	FSourceVTFHeader Header;
	int64 ImageDataOffset = 0;
	int64 LowResDataOffset = 0;
	int32 NumFaces = 1;
	bool bLoaded = false;
};
