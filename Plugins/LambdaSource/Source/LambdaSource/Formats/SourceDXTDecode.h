#pragma once

#include "CoreMinimal.h"
#include "Formats/SourceVTFFile.h"

/**
 * Software decode of the image formats Source ships textures in, for the few places the CPU needs pixel values
 * rather than a GPU upload - building a decal's height tile from its atlas, for instance. Not a general decoder:
 * only the formats HL2 content uses (DXT1/3/5 and the 24/32-bit uncompressed layouts).
 */
namespace SourceDXT
{
	/** Decodes one mip of a VTF to 8-bit RGBA, row-major, top-left first. Returns false for unsupported formats. */
	LAMBDASOURCE_API bool DecodeToRGBA(const FSourceVTFFile& VTF, int32 MipLevel, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight);
}
