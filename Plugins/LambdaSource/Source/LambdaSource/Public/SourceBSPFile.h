#pragma once

#include "CoreMinimal.h"
#include "SourceBSPFormat.h"

/** One entity from the BSP entity lump: an ordered list of key/value pairs (keys may repeat, e.g. outputs). */
struct LAMBDASOURCE_API FSourceEntity
{
	TArray<TPair<FString, FString>> Pairs;
	FString ClassName;

	bool Has(FStringView Key) const;
	FString Get(FStringView Key, const FString& Default = FString()) const;
	bool GetVector(FStringView Key, FVector3f& Out) const;
	float GetFloat(FStringView Key, float Default = 0.0f) const;
	int32 GetInt(FStringView Key, int32 Default = 0) const;
	FString ToString() const;
};

/**
 * Loads a compiled Source Engine BSP (VBSP 19-21) into memory and exposes the lumps needed for rendering/collision.
 * Geometry stays in Source coordinates and Hammer units; see FSourceCoords for conversion.
 */
class LAMBDASOURCE_API FSourceBSPFile
{
public:
	bool LoadFromFile(const FString& FilePath, FString* OutError = nullptr);
	/** Takes ownership of the buffer. */
	bool LoadFromMemory(TArray<uint8>&& InData, FString* OutError = nullptr);

	bool IsLoaded() const { return bLoaded; }
	int32 GetVersion() const { return Header.version; }
	int32 GetMapRevision() const { return Header.mapRevision; }
	int64 GetFileSize() const { return Data.Num(); }

	const SourceBSP::lump_t& GetLumpInfo(int32 LumpIndex) const { return Header.lumps[LumpIndex]; }
	bool HasLump(int32 LumpIndex) const;
	/** Raw bytes of a lump (empty view if absent/out of range). */
	TConstArrayView<uint8> GetLumpData(int32 LumpIndex) const;

	/** Copies a lump into an array of fixed-size records. Returns false if the lump size is not a multiple of sizeof(T). */
	template <typename T>
	bool ReadLumpArray(int32 LumpIndex, TArray<T>& Out, FString* OutError = nullptr) const
	{
		Out.Reset();
		const TConstArrayView<uint8> Bytes = GetLumpData(LumpIndex);
		if (Bytes.Num() == 0)
		{
			return true;
		}
		if (Bytes.Num() % sizeof(T) != 0)
		{
			if (OutError)
			{
				*OutError = FString::Printf(TEXT("Lump %d (%s) size %d is not a multiple of record size %d"),
					LumpIndex, SourceBSP::GetLumpName(LumpIndex), Bytes.Num(), (int32)sizeof(T));
			}
			return false;
		}
		const int32 Count = Bytes.Num() / sizeof(T);
		Out.SetNumUninitialized(Count);
		FMemory::Memcpy(Out.GetData(), Bytes.GetData(), Bytes.Num());
		return true;
	}

	/** Material name for a texdata index, e.g. "DEV/DEV_MEASUREGENERIC01" (as stored; upper case is common). */
	FString GetTexDataName(int32 TexDataIndex) const;
	/** Material name for a texinfo index (resolves texdata). */
	FString GetTexInfoMaterialName(int32 TexInfoIndex) const;

	/** All entities with the given classname. */
	TArray<const FSourceEntity*> FindEntities(FStringView ClassName) const;
	const FSourceEntity* FindFirstEntity(FStringView ClassName) const;
	const FSourceEntity* GetWorldspawn() const { return FindFirstEntity(TEXT("worldspawn")); }

	void LogSummary(const FString& Label) const;

	// ---- Parsed lumps (Source coordinates / Hammer units) ----
	TArray<SourceBSP::dplane_t> Planes;
	TArray<FVector3f> Vertices;
	TArray<SourceBSP::dedge_t> Edges;
	TArray<int32> SurfEdges;
	TArray<SourceBSP::dface_t> Faces;
	TArray<SourceBSP::dface_t> OriginalFaces;
	TArray<SourceBSP::texinfo_t> TexInfos;
	TArray<SourceBSP::dtexdata_t> TexDatas;
	TArray<int32> TexDataStringTable;
	TArray<uint8> TexDataStringData;
	TArray<SourceBSP::dmodel_t> Models;
	TArray<SourceBSP::dbrush_t> Brushes;
	TArray<SourceBSP::dbrushside_t> BrushSides;
	TArray<SourceBSP::dnode_t> Nodes;
	TArray<SourceBSP::dleaf_t> Leafs;
	TArray<uint16> LeafFaces;
	TArray<uint16> LeafBrushes;
	TArray<FVector3f> VertNormals;
	TArray<uint16> VertNormalIndices;
	TArray<SourceBSP::ddispinfo_t> DispInfos;
	TArray<SourceBSP::CDispVert> DispVerts;
	TArray<SourceBSP::CDispTri> DispTris;
	TArray<SourceBSP::dcubemapsample_t> Cubemaps;

	FString EntityText;
	TArray<FSourceEntity> Entities;

private:
	bool ParseLumps(FString* OutError);
	bool ParseEntities(FString* OutError);

	TArray<uint8> Data;
	SourceBSP::dheader_t Header{};
	bool bLoaded = false;
};
