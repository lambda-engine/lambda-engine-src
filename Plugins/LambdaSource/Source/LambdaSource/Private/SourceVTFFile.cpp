#include "SourceVTFFile.h"
#include "LambdaSourceModule.h"

namespace
{
	template <typename T>
	T ReadLE(const TArray<uint8>& Data, int64 Offset)
	{
		T Value{};
		if (Offset >= 0 && Offset + (int64)sizeof(T) <= Data.Num())
		{
			FMemory::Memcpy(&Value, Data.GetData() + Offset, sizeof(T));
		}
		return Value;
	}

	constexpr uint32 VTF_RSRC_LOW_RES_IMAGE = 0x01;
	constexpr uint32 VTF_RSRC_IMAGE = 0x30;
	constexpr int64 VTF_RESOURCE_DICTIONARY_OFFSET = 80; // resources start at 0x50 in 7.3+ files (see vtf.h comment)
}

bool FSourceVTFFile::Load(const TArray<uint8>& InData, FString* OutError)
{
	TArray<uint8> Copy = InData;
	return Load(MoveTemp(Copy), OutError);
}

bool FSourceVTFFile::Load(TArray<uint8>&& InData, FString* OutError)
{
	bLoaded = false;
	Data = MoveTemp(InData);
	Header = FSourceVTFHeader();

	auto Fail = [&](const FString& Msg)
	{
		if (OutError)
		{
			*OutError = Msg;
		}
		return false;
	};

	if (Data.Num() < 64)
	{
		return Fail(TEXT("File too small to be a VTF"));
	}
	if (!(Data[0] == 'V' && Data[1] == 'T' && Data[2] == 'F' && Data[3] == 0))
	{
		return Fail(TEXT("Bad VTF signature"));
	}

	Header.VersionMajor = ReadLE<int32>(Data, 4);
	Header.VersionMinor = ReadLE<int32>(Data, 8);
	Header.HeaderSize = ReadLE<uint32>(Data, 12);
	if (Header.VersionMajor != 7 || Header.VersionMinor < 0 || Header.VersionMinor > 5)
	{
		return Fail(FString::Printf(TEXT("Unsupported VTF version %d.%d"), Header.VersionMajor, Header.VersionMinor));
	}

	Header.Width = ReadLE<uint16>(Data, 16);
	Header.Height = ReadLE<uint16>(Data, 18);
	Header.Flags = ReadLE<uint32>(Data, 20);
	Header.NumFrames = FMath::Max<int32>(1, ReadLE<uint16>(Data, 24));
	Header.StartFrame = ReadLE<uint16>(Data, 26);
	Header.Reflectivity = FVector3f(ReadLE<float>(Data, 32), ReadLE<float>(Data, 36), ReadLE<float>(Data, 40));
	Header.BumpScale = ReadLE<float>(Data, 48);
	Header.ImageFormat = (ESourceImageFormat)ReadLE<int32>(Data, 52);
	Header.NumMipLevels = FMath::Max<int32>(1, ReadLE<uint8>(Data, 56));
	Header.LowResImageFormat = (ESourceImageFormat)ReadLE<int32>(Data, 57);
	Header.LowResWidth = ReadLE<uint8>(Data, 61);
	Header.LowResHeight = ReadLE<uint8>(Data, 62);
	Header.Depth = 1;
	if (Header.VersionMinor >= 2)
	{
		Header.Depth = FMath::Max<int32>(1, ReadLE<uint16>(Data, 63));
	}
	if (Header.VersionMinor >= 3)
	{
		Header.NumResources = ReadLE<uint32>(Data, 68);
	}

	if (Header.Width <= 0 || Header.Height <= 0)
	{
		return Fail(TEXT("VTF has zero size"));
	}
	if ((int32)Header.ImageFormat < 0 || (int32)Header.ImageFormat >= (int32)ESourceImageFormat::Count)
	{
		return Fail(FString::Printf(TEXT("Unknown VTF image format %d"), (int32)Header.ImageFormat));
	}

	// Cubemaps: 7 faces (6 + spheremap) before 7.5, 6 afterwards (VTFLib logic).
	NumFaces = 1;
	if (Header.Flags & FLAG_ENVMAP)
	{
		NumFaces = (Header.VersionMinor < 5 && Header.StartFrame != 0xFFFF) ? 7 : 6;
	}

	// Locate the image data.
	ImageDataOffset = -1;
	LowResDataOffset = -1;
	if (Header.VersionMinor >= 3)
	{
		for (int32 i = 0; i < Header.NumResources; ++i)
		{
			const int64 EntryOffset = VTF_RESOURCE_DICTIONARY_OFFSET + i * 8;
			if (EntryOffset + 8 > Data.Num())
			{
				break;
			}
			const uint32 Type = (uint32)Data[EntryOffset] | ((uint32)Data[EntryOffset + 1] << 8) | ((uint32)Data[EntryOffset + 2] << 16);
			const uint32 ResData = ReadLE<uint32>(Data, EntryOffset + 4);
			if (Type == VTF_RSRC_IMAGE)
			{
				ImageDataOffset = ResData;
			}
			else if (Type == VTF_RSRC_LOW_RES_IMAGE)
			{
				LowResDataOffset = ResData;
			}
		}
	}

	if (ImageDataOffset < 0)
	{
		// Either an older file, or a 7.3+ one that never wrote a resource dictionary. Tools do produce the
		// latter - a 7.4 header with numResources 0 - and the data is then laid out the way it always was:
		// the header, the low-res thumbnail, then the mip chain. Reading it that way costs nothing and is
		// checked against the file's length below, so a genuinely broken file still fails.
		int64 LowResBytes = 0;
		if ((int32)Header.LowResImageFormat >= 0 && Header.LowResWidth > 0 && Header.LowResHeight > 0)
		{
			LowResBytes = ComputeImageBytes(Header.LowResWidth, Header.LowResHeight, Header.LowResImageFormat);
		}
		LowResDataOffset = Header.HeaderSize;
		ImageDataOffset = Header.HeaderSize + LowResBytes;
	}

	// Validate total size.
	int64 TotalImageBytes = 0;
	for (int32 Mip = 0; Mip < Header.NumMipLevels; ++Mip)
	{
		int32 W, H;
		GetMipDimensions(Mip, W, H);
		TotalImageBytes += ComputeImageBytes(W, H, Header.ImageFormat) * Header.NumFrames * NumFaces * Header.Depth;
	}
	if (ImageDataOffset + TotalImageBytes > Data.Num())
	{
		return Fail(FString::Printf(TEXT("VTF image data truncated (need %lld bytes at offset %lld, file is %d bytes)"),
			TotalImageBytes, ImageDataOffset, Data.Num()));
	}

	bLoaded = true;
	return true;
}

void FSourceVTFFile::GetMipDimensions(int32 MipLevel, int32& OutWidth, int32& OutHeight) const
{
	OutWidth = FMath::Max(1, Header.Width >> MipLevel);
	OutHeight = FMath::Max(1, Header.Height >> MipLevel);
}

int64 FSourceVTFFile::ComputeMipOffset(int32 MipLevel, int32 Frame, int32 Face, int32 Slice) const
{
	// Layout: for each mip (smallest first) { for each frame { for each face { for each slice { image } } } }
	int64 Offset = ImageDataOffset;
	for (int32 Mip = Header.NumMipLevels - 1; Mip > MipLevel; --Mip)
	{
		int32 W, H;
		GetMipDimensions(Mip, W, H);
		Offset += ComputeImageBytes(W, H, Header.ImageFormat) * Header.NumFrames * NumFaces * Header.Depth;
	}
	int32 W, H;
	GetMipDimensions(MipLevel, W, H);
	const int64 ImageBytes = ComputeImageBytes(W, H, Header.ImageFormat);
	Offset += (((int64)Frame * NumFaces + Face) * Header.Depth + Slice) * ImageBytes;
	return Offset;
}

bool FSourceVTFFile::GetMipData(int32 MipLevel, int32 Frame, int32 Face, int32 Slice, TConstArrayView<uint8>& OutData) const
{
	if (!bLoaded || MipLevel < 0 || MipLevel >= Header.NumMipLevels || Frame < 0 || Frame >= Header.NumFrames ||
		Face < 0 || Face >= NumFaces || Slice < 0 || Slice >= Header.Depth)
	{
		return false;
	}
	int32 W, H;
	GetMipDimensions(MipLevel, W, H);
	const int64 Bytes = ComputeImageBytes(W, H, Header.ImageFormat);
	const int64 Offset = ComputeMipOffset(MipLevel, Frame, Face, Slice);
	if (Offset < 0 || Offset + Bytes > Data.Num())
	{
		return false;
	}
	OutData = TConstArrayView<uint8>(Data.GetData() + Offset, (int32)Bytes);
	return true;
}

bool FSourceVTFFile::IsCompressedFormat(ESourceImageFormat Format)
{
	switch (Format)
	{
	case ESourceImageFormat::DXT1:
	case ESourceImageFormat::DXT1_ONEBITALPHA:
	case ESourceImageFormat::DXT3:
	case ESourceImageFormat::DXT5:
	case ESourceImageFormat::ATI1N:
	case ESourceImageFormat::ATI2N:
		return true;
	default:
		return false;
	}
}

int32 FSourceVTFFile::GetBlockBytes(ESourceImageFormat Format)
{
	switch (Format)
	{
	case ESourceImageFormat::DXT1:
	case ESourceImageFormat::DXT1_ONEBITALPHA:
	case ESourceImageFormat::ATI1N:
		return 8;
	case ESourceImageFormat::DXT3:
	case ESourceImageFormat::DXT5:
	case ESourceImageFormat::ATI2N:
		return 16;
	default:
		return 0;
	}
}

int32 FSourceVTFFile::GetBytesPerPixel(ESourceImageFormat Format)
{
	switch (Format)
	{
	case ESourceImageFormat::RGBA8888:
	case ESourceImageFormat::ABGR8888:
	case ESourceImageFormat::ARGB8888:
	case ESourceImageFormat::BGRA8888:
	case ESourceImageFormat::BGRX8888:
	case ESourceImageFormat::UVWQ8888:
	case ESourceImageFormat::UVLX8888:
	case ESourceImageFormat::RGBX8888:
	case ESourceImageFormat::R32F:
		return 4;
	case ESourceImageFormat::RGB888:
	case ESourceImageFormat::BGR888:
	case ESourceImageFormat::RGB888_BLUESCREEN:
	case ESourceImageFormat::BGR888_BLUESCREEN:
		return 3;
	case ESourceImageFormat::RGB565:
	case ESourceImageFormat::BGR565:
	case ESourceImageFormat::BGRX5551:
	case ESourceImageFormat::BGRA4444:
	case ESourceImageFormat::BGRA5551:
	case ESourceImageFormat::IA88:
	case ESourceImageFormat::UV88:
		return 2;
	case ESourceImageFormat::I8:
	case ESourceImageFormat::P8:
	case ESourceImageFormat::A8:
		return 1;
	case ESourceImageFormat::RGBA16161616F:
	case ESourceImageFormat::RGBA16161616:
	case ESourceImageFormat::RG3232F:
		return 8;
	case ESourceImageFormat::RG1616F:
		return 4;
	case ESourceImageFormat::RGB323232F:
		return 12;
	case ESourceImageFormat::RGBA32323232F:
		return 16;
	default:
		return 0;
	}
}

int64 FSourceVTFFile::ComputeImageBytes(int32 Width, int32 Height, ESourceImageFormat Format)
{
	if (IsCompressedFormat(Format))
	{
		const int64 BlocksX = FMath::Max(1, (Width + 3) / 4);
		const int64 BlocksY = FMath::Max(1, (Height + 3) / 4);
		return BlocksX * BlocksY * GetBlockBytes(Format);
	}
	return (int64)Width * Height * GetBytesPerPixel(Format);
}

const TCHAR* FSourceVTFFile::GetFormatName(ESourceImageFormat Format)
{
	switch (Format)
	{
	case ESourceImageFormat::RGBA8888: return TEXT("RGBA8888");
	case ESourceImageFormat::ABGR8888: return TEXT("ABGR8888");
	case ESourceImageFormat::RGB888: return TEXT("RGB888");
	case ESourceImageFormat::BGR888: return TEXT("BGR888");
	case ESourceImageFormat::RGB565: return TEXT("RGB565");
	case ESourceImageFormat::I8: return TEXT("I8");
	case ESourceImageFormat::IA88: return TEXT("IA88");
	case ESourceImageFormat::P8: return TEXT("P8");
	case ESourceImageFormat::A8: return TEXT("A8");
	case ESourceImageFormat::RGB888_BLUESCREEN: return TEXT("RGB888_BLUESCREEN");
	case ESourceImageFormat::BGR888_BLUESCREEN: return TEXT("BGR888_BLUESCREEN");
	case ESourceImageFormat::ARGB8888: return TEXT("ARGB8888");
	case ESourceImageFormat::BGRA8888: return TEXT("BGRA8888");
	case ESourceImageFormat::DXT1: return TEXT("DXT1");
	case ESourceImageFormat::DXT3: return TEXT("DXT3");
	case ESourceImageFormat::DXT5: return TEXT("DXT5");
	case ESourceImageFormat::BGRX8888: return TEXT("BGRX8888");
	case ESourceImageFormat::BGR565: return TEXT("BGR565");
	case ESourceImageFormat::BGRX5551: return TEXT("BGRX5551");
	case ESourceImageFormat::BGRA4444: return TEXT("BGRA4444");
	case ESourceImageFormat::DXT1_ONEBITALPHA: return TEXT("DXT1_ONEBITALPHA");
	case ESourceImageFormat::BGRA5551: return TEXT("BGRA5551");
	case ESourceImageFormat::UV88: return TEXT("UV88");
	case ESourceImageFormat::UVWQ8888: return TEXT("UVWQ8888");
	case ESourceImageFormat::RGBA16161616F: return TEXT("RGBA16161616F");
	case ESourceImageFormat::RGBA16161616: return TEXT("RGBA16161616");
	case ESourceImageFormat::UVLX8888: return TEXT("UVLX8888");
	case ESourceImageFormat::R32F: return TEXT("R32F");
	case ESourceImageFormat::RGB323232F: return TEXT("RGB323232F");
	case ESourceImageFormat::RGBA32323232F: return TEXT("RGBA32323232F");
	case ESourceImageFormat::RG1616F: return TEXT("RG1616F");
	case ESourceImageFormat::RG3232F: return TEXT("RG3232F");
	case ESourceImageFormat::RGBX8888: return TEXT("RGBX8888");
	case ESourceImageFormat::NullFormat: return TEXT("NULL");
	case ESourceImageFormat::ATI2N: return TEXT("ATI2N");
	case ESourceImageFormat::ATI1N: return TEXT("ATI1N");
	default: return TEXT("UNKNOWN");
	}
}
