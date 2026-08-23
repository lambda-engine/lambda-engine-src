#include "SourceBSPFile.h"
#include "SourceKeyValues.h"
#include "SourceCoordinates.h"
#include "LambdaSourceModule.h"
#include "Misc/FileHelper.h"

// ---------------------------------------------------------------------------------------------------------------------
// Lump names
// ---------------------------------------------------------------------------------------------------------------------

const TCHAR* SourceBSP::GetLumpName(int32 LumpIndex)
{
	static const TCHAR* Names[HEADER_LUMPS] =
	{
		TEXT("ENTITIES"), TEXT("PLANES"), TEXT("TEXDATA"), TEXT("VERTEXES"), TEXT("VISIBILITY"), TEXT("NODES"), TEXT("TEXINFO"), TEXT("FACES"),
		TEXT("LIGHTING"), TEXT("OCCLUSION"), TEXT("LEAFS"), TEXT("FACEIDS"), TEXT("EDGES"), TEXT("SURFEDGES"), TEXT("MODELS"), TEXT("WORLDLIGHTS"),
		TEXT("LEAFFACES"), TEXT("LEAFBRUSHES"), TEXT("BRUSHES"), TEXT("BRUSHSIDES"), TEXT("AREAS"), TEXT("AREAPORTALS"), TEXT("PORTALS"), TEXT("CLUSTERS"),
		TEXT("PORTALVERTS"), TEXT("CLUSTERPORTALS"), TEXT("DISPINFO"), TEXT("ORIGINALFACES"), TEXT("PHYSDISP"), TEXT("PHYSCOLLIDE"), TEXT("VERTNORMALS"), TEXT("VERTNORMALINDICES"),
		TEXT("DISP_LIGHTMAP_ALPHAS"), TEXT("DISP_VERTS"), TEXT("DISP_LIGHTMAP_SAMPLE_POSITIONS"), TEXT("GAME_LUMP"), TEXT("LEAFWATERDATA"), TEXT("PRIMITIVES"), TEXT("PRIMVERTS"), TEXT("PRIMINDICES"),
		TEXT("PAKFILE"), TEXT("CLIPPORTALVERTS"), TEXT("CUBEMAPS"), TEXT("TEXDATA_STRING_DATA"), TEXT("TEXDATA_STRING_TABLE"), TEXT("OVERLAYS"), TEXT("LEAFMINDISTTOWATER"), TEXT("FACE_MACRO_TEXTURE_INFO"),
		TEXT("DISP_TRIS"), TEXT("PHYSCOLLIDESURFACE"), TEXT("WATEROVERLAYS"), TEXT("LEAF_AMBIENT_INDEX_HDR"), TEXT("LEAF_AMBIENT_INDEX"), TEXT("LIGHTING_HDR"), TEXT("WORLDLIGHTS_HDR"), TEXT("LEAF_AMBIENT_LIGHTING_HDR"),
		TEXT("LEAF_AMBIENT_LIGHTING"), TEXT("XZIPPAKFILE"), TEXT("FACES_HDR"), TEXT("MAP_FLAGS"), TEXT("OVERLAY_FADES"), TEXT("OVERLAY_SYSTEM_LEVELS"), TEXT("PHYSLEVEL"), TEXT("DISP_MULTIBLEND"),
	};
	return (LumpIndex >= 0 && LumpIndex < HEADER_LUMPS) ? Names[LumpIndex] : TEXT("INVALID");
}

// ---------------------------------------------------------------------------------------------------------------------
// FSourceEntity
// ---------------------------------------------------------------------------------------------------------------------

bool FSourceEntity::Has(FStringView Key) const
{
	for (const TPair<FString, FString>& Pair : Pairs)
	{
		if (Key.Equals(Pair.Key, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

FString FSourceEntity::Get(FStringView Key, const FString& Default) const
{
	for (const TPair<FString, FString>& Pair : Pairs)
	{
		if (Key.Equals(Pair.Key, ESearchCase::IgnoreCase))
		{
			return Pair.Value;
		}
	}
	return Default;
}

bool FSourceEntity::GetVector(FStringView Key, FVector3f& Out) const
{
	const FString Value = Get(Key);
	return !Value.IsEmpty() && FSourceCoords::ParseVector(Value, Out);
}

float FSourceEntity::GetFloat(FStringView Key, float Default) const
{
	const FString Value = Get(Key);
	return Value.IsEmpty() ? Default : FCString::Atof(*Value);
}

int32 FSourceEntity::GetInt(FStringView Key, int32 Default) const
{
	const FString Value = Get(Key);
	return Value.IsEmpty() ? Default : FCString::Atoi(*Value);
}

FString FSourceEntity::ToString() const
{
	FString Result = TEXT("{ ");
	for (const TPair<FString, FString>& Pair : Pairs)
	{
		Result += FString::Printf(TEXT("\"%s\"=\"%s\" "), *Pair.Key, *Pair.Value);
	}
	Result += TEXT("}");
	return Result;
}

// ---------------------------------------------------------------------------------------------------------------------
// FSourceBSPFile
// ---------------------------------------------------------------------------------------------------------------------

bool FSourceBSPFile::LoadFromFile(const FString& FilePath, FString* OutError)
{
	TArray<uint8> FileData;
	if (!FFileHelper::LoadFileToArray(FileData, *FilePath))
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("Could not read file '%s'"), *FilePath);
		}
		return false;
	}
	return LoadFromMemory(MoveTemp(FileData), OutError);
}

bool FSourceBSPFile::LoadFromMemory(TArray<uint8>&& InData, FString* OutError)
{
	bLoaded = false;
	Data = MoveTemp(InData);

	auto Fail = [&](const FString& Msg)
	{
		if (OutError)
		{
			*OutError = Msg;
		}
		return false;
	};

	if (Data.Num() < (int32)sizeof(SourceBSP::dheader_t))
	{
		return Fail(TEXT("File too small to be a BSP"));
	}
	FMemory::Memcpy(&Header, Data.GetData(), sizeof(SourceBSP::dheader_t));

	if (Header.ident != SourceBSP::IDBSPHEADER)
	{
		return Fail(TEXT("Bad BSP identifier (expected 'VBSP')"));
	}
	if (Header.version < SourceBSP::MIN_SUPPORTED_VERSION || Header.version > SourceBSP::MAX_SUPPORTED_VERSION)
	{
		return Fail(FString::Printf(TEXT("Unsupported BSP version %d (supported: %d-%d)"), Header.version,
			SourceBSP::MIN_SUPPORTED_VERSION, SourceBSP::MAX_SUPPORTED_VERSION));
	}

	// Validate lump table.
	for (int32 i = 0; i < SourceBSP::HEADER_LUMPS; ++i)
	{
		const SourceBSP::lump_t& Lump = Header.lumps[i];
		if (Lump.filelen < 0 || Lump.fileofs < 0 || (Lump.filelen > 0 && (int64)Lump.fileofs + Lump.filelen > Data.Num()))
		{
			return Fail(FString::Printf(TEXT("Lump %d (%s) is out of bounds (ofs=%d len=%d file=%d)"),
				i, SourceBSP::GetLumpName(i), Lump.fileofs, Lump.filelen, Data.Num()));
		}
	}

	if (!ParseLumps(OutError))
	{
		return false;
	}
	if (!ParseEntities(OutError))
	{
		return false;
	}

	bLoaded = true;
	return true;
}

bool FSourceBSPFile::HasLump(int32 LumpIndex) const
{
	return LumpIndex >= 0 && LumpIndex < SourceBSP::HEADER_LUMPS && Header.lumps[LumpIndex].filelen > 0;
}

TConstArrayView<uint8> FSourceBSPFile::GetLumpData(int32 LumpIndex) const
{
	if (!HasLump(LumpIndex))
	{
		return TConstArrayView<uint8>();
	}
	const SourceBSP::lump_t& Lump = Header.lumps[LumpIndex];
	return TConstArrayView<uint8>(Data.GetData() + Lump.fileofs, Lump.filelen);
}

bool FSourceBSPFile::ParseLumps(FString* OutError)
{
	using namespace SourceBSP;

	if (!ReadLumpArray(LUMP_PLANES, Planes, OutError)) return false;
	if (!ReadLumpArray(LUMP_VERTEXES, Vertices, OutError)) return false;
	if (!ReadLumpArray(LUMP_EDGES, Edges, OutError)) return false;
	if (!ReadLumpArray(LUMP_SURFEDGES, SurfEdges, OutError)) return false;
	if (!ReadLumpArray(LUMP_FACES, Faces, OutError)) return false;
	if (!ReadLumpArray(LUMP_ORIGINALFACES, OriginalFaces, OutError)) return false;
	if (!ReadLumpArray(LUMP_TEXINFO, TexInfos, OutError)) return false;
	if (!ReadLumpArray(LUMP_TEXDATA, TexDatas, OutError)) return false;
	if (!ReadLumpArray(LUMP_TEXDATA_STRING_TABLE, TexDataStringTable, OutError)) return false;
	if (!ReadLumpArray(LUMP_TEXDATA_STRING_DATA, TexDataStringData, OutError)) return false;
	if (!ReadLumpArray(LUMP_MODELS, Models, OutError)) return false;
	if (!ReadLumpArray(LUMP_BRUSHES, Brushes, OutError)) return false;
	if (!ReadLumpArray(LUMP_BRUSHSIDES, BrushSides, OutError)) return false;
	if (!ReadLumpArray(LUMP_NODES, Nodes, OutError)) return false;
	if (!ReadLumpArray(LUMP_LEAFFACES, LeafFaces, OutError)) return false;
	if (!ReadLumpArray(LUMP_LEAFBRUSHES, LeafBrushes, OutError)) return false;
	if (!ReadLumpArray(LUMP_VERTNORMALS, VertNormals, OutError)) return false;
	if (!ReadLumpArray(LUMP_VERTNORMALINDICES, VertNormalIndices, OutError)) return false;
	if (!ReadLumpArray(LUMP_DISPINFO, DispInfos, OutError)) return false;
	if (!ReadLumpArray(LUMP_DISP_VERTS, DispVerts, OutError)) return false;
	if (!ReadLumpArray(LUMP_DISP_TRIS, DispTris, OutError)) return false;
	if (!ReadLumpArray(LUMP_CUBEMAPS, Cubemaps, OutError)) return false;

	// Leafs: version 0 embeds the ambient light cube (56 bytes), version 1 is 32 bytes.
	if (Header.lumps[LUMP_LEAFS].version == 0 && HasLump(LUMP_LEAFS))
	{
		TArray<dleaf_v0_t> OldLeafs;
		if (!ReadLumpArray(LUMP_LEAFS, OldLeafs, OutError)) return false;
		Leafs.SetNumUninitialized(OldLeafs.Num());
		for (int32 i = 0; i < OldLeafs.Num(); ++i)
		{
			FMemory::Memcpy(&Leafs[i], &OldLeafs[i], sizeof(dleaf_t));
		}
	}
	else
	{
		if (!ReadLumpArray(LUMP_LEAFS, Leafs, OutError)) return false;
	}

	if (Models.Num() == 0)
	{
		if (OutError)
		{
			*OutError = TEXT("BSP has no models (no world geometry)");
		}
		return false;
	}

	// Basic index validation so the geometry builder can trust the data.
	for (int32 i = 0; i < Faces.Num(); ++i)
	{
		const dface_t& Face = Faces[i];
		if (Face.firstedge < 0 || Face.numedges < 0 || Face.firstedge + Face.numedges > SurfEdges.Num() ||
			!Planes.IsValidIndex(Face.planenum) || (Face.texinfo >= 0 && !TexInfos.IsValidIndex(Face.texinfo)))
		{
			if (OutError)
			{
				*OutError = FString::Printf(TEXT("Face %d references out-of-range data"), i);
			}
			return false;
		}
	}
	for (int32 i = 0; i < SurfEdges.Num(); ++i)
	{
		const int32 EdgeIndex = FMath::Abs(SurfEdges[i]);
		if (!Edges.IsValidIndex(EdgeIndex))
		{
			if (OutError)
			{
				*OutError = FString::Printf(TEXT("Surfedge %d references invalid edge %d"), i, EdgeIndex);
			}
			return false;
		}
	}
	for (int32 i = 0; i < Edges.Num(); ++i)
	{
		if (!Vertices.IsValidIndex(Edges[i].v[0]) || !Vertices.IsValidIndex(Edges[i].v[1]))
		{
			if (OutError)
			{
				*OutError = FString::Printf(TEXT("Edge %d references invalid vertex"), i);
			}
			return false;
		}
	}
	return true;
}

bool FSourceBSPFile::ParseEntities(FString* OutError)
{
	Entities.Reset();
	EntityText.Reset();

	const TConstArrayView<uint8> Bytes = GetLumpData(SourceBSP::LUMP_ENTITIES);
	if (Bytes.Num() == 0)
	{
		return true;
	}

	// The lump is null-terminated ANSI text.
	int32 TextLen = Bytes.Num();
	while (TextLen > 0 && Bytes[TextLen - 1] == 0)
	{
		--TextLen;
	}
	FFileHelper::BufferToString(EntityText, Bytes.GetData(), TextLen);

	TArray<FSourceKeyValues> Blocks;
	FString ParseError;
	if (!FSourceKeyValues::ParseText(EntityText, Blocks, &ParseError))
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("Entity lump parse error: %s"), *ParseError);
		}
		return false;
	}

	Entities.Reserve(Blocks.Num());
	for (const FSourceKeyValues& Block : Blocks)
	{
		if (!Block.IsSection())
		{
			continue;
		}
		FSourceEntity& Entity = Entities.AddDefaulted_GetRef();
		Entity.Pairs.Reserve(Block.Children.Num());
		for (const FSourceKeyValues& Pair : Block.Children)
		{
			if (!Pair.IsSection())
			{
				Entity.Pairs.Emplace(Pair.Key, Pair.Value);
			}
		}
		Entity.ClassName = Entity.Get(TEXT("classname"));
	}
	return true;
}

FString FSourceBSPFile::GetTexDataName(int32 TexDataIndex) const
{
	if (!TexDatas.IsValidIndex(TexDataIndex))
	{
		return FString();
	}
	const int32 StringTableId = TexDatas[TexDataIndex].nameStringTableID;
	if (!TexDataStringTable.IsValidIndex(StringTableId))
	{
		return FString();
	}
	const int32 Offset = TexDataStringTable[StringTableId];
	if (Offset < 0 || Offset >= TexDataStringData.Num())
	{
		return FString();
	}
	int32 End = Offset;
	while (End < TexDataStringData.Num() && TexDataStringData[End] != 0)
	{
		++End;
	}
	FString Name;
	FFileHelper::BufferToString(Name, TexDataStringData.GetData() + Offset, End - Offset);
	return Name;
}

FString FSourceBSPFile::GetTexInfoMaterialName(int32 TexInfoIndex) const
{
	if (!TexInfos.IsValidIndex(TexInfoIndex))
	{
		return FString();
	}
	return GetTexDataName(TexInfos[TexInfoIndex].texdata);
}

TArray<const FSourceEntity*> FSourceBSPFile::FindEntities(FStringView ClassName) const
{
	TArray<const FSourceEntity*> Result;
	for (const FSourceEntity& Entity : Entities)
	{
		if (ClassName.Equals(Entity.ClassName, ESearchCase::IgnoreCase))
		{
			Result.Add(&Entity);
		}
	}
	return Result;
}

const FSourceEntity* FSourceBSPFile::FindFirstEntity(FStringView ClassName) const
{
	for (const FSourceEntity& Entity : Entities)
	{
		if (ClassName.Equals(Entity.ClassName, ESearchCase::IgnoreCase))
		{
			return &Entity;
		}
	}
	return nullptr;
}

void FSourceBSPFile::LogSummary(const FString& Label) const
{
	UE_LOG(LogLambdaSource, Log, TEXT("BSP '%s': VBSP v%d rev %d, %lld bytes"), *Label, Header.version, Header.mapRevision, GetFileSize());
	for (int32 i = 0; i < SourceBSP::HEADER_LUMPS; ++i)
	{
		const SourceBSP::lump_t& Lump = Header.lumps[i];
		if (Lump.filelen > 0)
		{
			UE_LOG(LogLambdaSource, Verbose, TEXT("  lump %2d %-32s ofs=%8d len=%8d ver=%d"), i, SourceBSP::GetLumpName(i), Lump.fileofs, Lump.filelen, Lump.version);
		}
	}
	UE_LOG(LogLambdaSource, Log, TEXT("  models=%d faces=%d origfaces=%d verts=%d edges=%d surfedges=%d planes=%d texinfos=%d texdatas=%d brushes=%d brushsides=%d nodes=%d leafs=%d disps=%d dispverts=%d entities=%d"),
		Models.Num(), Faces.Num(), OriginalFaces.Num(), Vertices.Num(), Edges.Num(), SurfEdges.Num(), Planes.Num(), TexInfos.Num(), TexDatas.Num(),
		Brushes.Num(), BrushSides.Num(), Nodes.Num(), Leafs.Num(), DispInfos.Num(), DispVerts.Num(), Entities.Num());
	for (int32 i = 0; i < TexDatas.Num(); ++i)
	{
		UE_LOG(LogLambdaSource, Verbose, TEXT("  texdata %d: %s (%dx%d)"), i, *GetTexDataName(i), TexDatas[i].width, TexDatas[i].height);
	}
	TMap<FString, int32> ClassCounts;
	for (const FSourceEntity& Entity : Entities)
	{
		ClassCounts.FindOrAdd(Entity.ClassName)++;
	}
	for (const TPair<FString, int32>& Pair : ClassCounts)
	{
		UE_LOG(LogLambdaSource, Verbose, TEXT("  entity class %s x%d"), *Pair.Key, Pair.Value);
	}
}
