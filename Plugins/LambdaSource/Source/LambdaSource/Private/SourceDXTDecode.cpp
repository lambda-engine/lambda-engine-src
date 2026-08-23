#include "SourceDXTDecode.h"

namespace
{
	FColor Unpack565(uint16 C)
	{
		return FColor(
			(uint8)(((C >> 11) & 31) * 255 / 31),
			(uint8)(((C >> 5) & 63) * 255 / 63),
			(uint8)((C & 31) * 255 / 31),
			255);
	}

	/** The 8-byte colour block shared by DXT1 (the whole block) and DXT3/DXT5 (their second half). */
	void DecodeColorBlock(const uint8* Block, bool bDXT1, FColor Out[16])
	{
		const uint16 C0 = Block[0] | (Block[1] << 8);
		const uint16 C1 = Block[2] | (Block[3] << 8);
		const uint32 Bits = Block[4] | (Block[5] << 8) | (Block[6] << 16) | (Block[7] << 24);

		FColor Palette[4];
		Palette[0] = Unpack565(C0);
		Palette[1] = Unpack565(C1);
		if (!bDXT1 || C0 > C1)
		{
			Palette[2] = FColor((2 * Palette[0].R + Palette[1].R) / 3, (2 * Palette[0].G + Palette[1].G) / 3, (2 * Palette[0].B + Palette[1].B) / 3, 255);
			Palette[3] = FColor((Palette[0].R + 2 * Palette[1].R) / 3, (Palette[0].G + 2 * Palette[1].G) / 3, (Palette[0].B + 2 * Palette[1].B) / 3, 255);
		}
		else
		{
			// DXT1's 1-bit-alpha mode: the fourth entry is transparent black.
			Palette[2] = FColor((Palette[0].R + Palette[1].R) / 2, (Palette[0].G + Palette[1].G) / 2, (Palette[0].B + Palette[1].B) / 2, 255);
			Palette[3] = FColor(0, 0, 0, 0);
		}
		for (int32 i = 0; i < 16; ++i)
		{
			Out[i] = Palette[(Bits >> (2 * i)) & 3];
		}
	}

	/** DXT5's 8-byte interpolated-alpha block. */
	void DecodeAlphaBlockDXT5(const uint8* Block, uint8 Out[16])
	{
		const uint8 A0 = Block[0], A1 = Block[1];
		uint8 Palette[8];
		Palette[0] = A0;
		Palette[1] = A1;
		if (A0 > A1)
		{
			for (int32 i = 1; i < 7; ++i) { Palette[i + 1] = (uint8)(((7 - i) * A0 + i * A1) / 7); }
		}
		else
		{
			for (int32 i = 1; i < 5; ++i) { Palette[i + 1] = (uint8)(((5 - i) * A0 + i * A1) / 5); }
			Palette[6] = 0;
			Palette[7] = 255;
		}
		uint64 Bits = 0;
		for (int32 i = 0; i < 6; ++i) { Bits |= (uint64)Block[2 + i] << (8 * i); }
		for (int32 i = 0; i < 16; ++i)
		{
			Out[i] = Palette[(Bits >> (3 * i)) & 7];
		}
	}
}

bool SourceDXT::DecodeToRGBA(const FSourceVTFFile& VTF, int32 MipLevel, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight)
{
	VTF.GetMipDimensions(MipLevel, OutWidth, OutHeight);
	TConstArrayView<uint8> Data;
	if (OutWidth <= 0 || OutHeight <= 0 || !VTF.GetMipData(MipLevel, 0, 0, 0, Data))
	{
		return false;
	}
	OutPixels.SetNumUninitialized(OutWidth * OutHeight);

	const ESourceImageFormat Format = VTF.GetFormat();
	switch (Format)
	{
	case ESourceImageFormat::DXT1:
	case ESourceImageFormat::DXT3:
	case ESourceImageFormat::DXT5:
	{
		const bool bDXT1 = Format == ESourceImageFormat::DXT1;
		const int32 BlockBytes = bDXT1 ? 8 : 16;
		const int32 BlocksX = (OutWidth + 3) / 4;
		const int32 BlocksY = (OutHeight + 3) / 4;
		if (Data.Num() < BlocksX * BlocksY * BlockBytes)
		{
			return false;
		}
		for (int32 By = 0; By < BlocksY; ++By)
		{
			for (int32 Bx = 0; Bx < BlocksX; ++Bx)
			{
				const uint8* Block = Data.GetData() + (By * BlocksX + Bx) * BlockBytes;
				FColor Colors[16];
				uint8 Alpha[16];
				DecodeColorBlock(bDXT1 ? Block : Block + 8, bDXT1, Colors);
				if (Format == ESourceImageFormat::DXT5)
				{
					DecodeAlphaBlockDXT5(Block, Alpha);
				}
				else if (Format == ESourceImageFormat::DXT3)
				{
					for (int32 i = 0; i < 16; ++i)
					{
						const uint8 Nibble = (Block[i / 2] >> ((i & 1) * 4)) & 15;
						Alpha[i] = (uint8)(Nibble * 17);
					}
				}
				for (int32 i = 0; i < 16; ++i)
				{
					const int32 X = Bx * 4 + (i & 3);
					const int32 Y = By * 4 + (i >> 2);
					if (X < OutWidth && Y < OutHeight)
					{
						FColor C = Colors[i];
						if (!bDXT1) { C.A = Alpha[i]; }
						OutPixels[Y * OutWidth + X] = C;
					}
				}
			}
		}
		return true;
	}
	case ESourceImageFormat::RGBA8888:
	case ESourceImageFormat::BGRA8888:
	case ESourceImageFormat::ARGB8888:
	case ESourceImageFormat::ABGR8888:
	{
		if (Data.Num() < OutWidth * OutHeight * 4) { return false; }
		for (int32 i = 0; i < OutWidth * OutHeight; ++i)
		{
			const uint8* P = Data.GetData() + i * 4;
			switch (Format)
			{
			case ESourceImageFormat::RGBA8888: OutPixels[i] = FColor(P[0], P[1], P[2], P[3]); break;
			case ESourceImageFormat::BGRA8888: OutPixels[i] = FColor(P[2], P[1], P[0], P[3]); break;
			case ESourceImageFormat::ARGB8888: OutPixels[i] = FColor(P[1], P[2], P[3], P[0]); break;
			default:                           OutPixels[i] = FColor(P[3], P[2], P[1], P[0]); break;	// ABGR
			}
		}
		return true;
	}
	case ESourceImageFormat::RGB888:
	case ESourceImageFormat::BGR888:
	{
		if (Data.Num() < OutWidth * OutHeight * 3) { return false; }
		for (int32 i = 0; i < OutWidth * OutHeight; ++i)
		{
			const uint8* P = Data.GetData() + i * 3;
			OutPixels[i] = Format == ESourceImageFormat::RGB888 ? FColor(P[0], P[1], P[2], 255) : FColor(P[2], P[1], P[0], 255);
		}
		return true;
	}
	default:
		return false;
	}
}
