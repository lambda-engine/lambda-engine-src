#include "SourceMDLFile.h"
#include "LambdaFileSystem.h"
#include "LambdaSourceModule.h"
#include "SourceCoordinates.h"
#include "Misc/Paths.h"

namespace
{
	// ---- studio.h on-disk layouts (v44-v49). Offsets verified against the shipped HL2 models. ----
	namespace MDL
	{
		constexpr int32 OFF_ID = 0, OFF_VERSION = 4, OFF_CHECKSUM = 8, OFF_NAME = 12, OFF_LENGTH = 76;
		constexpr int32 OFF_NUMBONES = 156, OFF_BONEINDEX = 160;
		constexpr int32 OFF_NUMTEXTURES = 204, OFF_TEXTUREINDEX = 208;
		constexpr int32 OFF_NUMCDTEXTURES = 212, OFF_CDTEXTUREINDEX = 216;
		constexpr int32 OFF_NUMSKINREF = 220, OFF_NUMSKINFAMILIES = 224, OFF_SKININDEX = 228;
		constexpr int32 OFF_NUMBODYPARTS = 232, OFF_BODYPARTINDEX = 236;

		constexpr int32 SIZE_TEXTURE = 64;		// mstudiotexture_t
		constexpr int32 SIZE_BODYPART = 16;		// mstudiobodyparts_t
		constexpr int32 SIZE_MODEL = 148;		// mstudiomodel_t
		constexpr int32 SIZE_MESH = 116;		// mstudiomesh_t
		// mstudiomodel_t: name[64], type, boundingradius, nummeshes, meshindex, numvertices, vertexindex, ...
		constexpr int32 MODEL_OFF_NUMMESHES = 72;
	}

	namespace VVD
	{
		constexpr int32 OFF_ID = 0, OFF_VERSION = 4, OFF_CHECKSUM = 8, OFF_NUMLODS = 12;
		constexpr int32 OFF_NUMLODVERTEXES = 16;	// int[8]
		constexpr int32 OFF_NUMFIXUPS = 48, OFF_FIXUPTABLESTART = 52, OFF_VERTEXDATASTART = 56, OFF_TANGENTDATASTART = 60;
		constexpr int32 SIZE_VERTEX = 48;			// mstudiovertex_t: boneweights(16) pos(12) normal(12) uv(8)
		constexpr int32 VERTEX_OFF_POS = 16, VERTEX_OFF_NORMAL = 28, VERTEX_OFF_UV = 40;
		constexpr int32 SIZE_FIXUP = 12;			// vertexFileFixup_t
	}

	// VTX structures are #pragma pack(1).
	namespace VTX
	{
		constexpr int32 OFF_VERSION = 0, OFF_CHECKSUM = 16, OFF_NUMLODS = 20;
		constexpr int32 OFF_NUMBODYPARTS = 28, OFF_BODYPARTOFFSET = 32;
		constexpr int32 SIZE_BODYPART = 8;		// numModels, modelOffset
		constexpr int32 SIZE_MODEL = 8;			// numLODs, lodOffset
		constexpr int32 SIZE_LOD = 12;			// numMeshes, meshOffset, switchPoint
		constexpr int32 SIZE_MESH = 9;			// numStripGroups, stripGroupHeaderOffset, flags
		constexpr int32 SIZE_STRIPGROUP = 25;	// numVerts, vertOffset, numIndices, indexOffset, numStrips, stripOffset, flags
		constexpr int32 SIZE_VERTEX = 9;		// boneWeightIndex[3], numBones, origMeshVertID, boneID[3]
		constexpr int32 VERTEX_OFF_ORIGMESHVERTID = 4;
		constexpr int32 SIZE_STRIP = 27;		// numIndices, indexOffset, numVerts, vertOffset, numBones, flags, numBoneStateChanges, boneStateChangeOffset

		constexpr uint8 STRIP_IS_TRILIST = 0x01;
	}

	template <typename T>
	bool Peek(const TArray<uint8>& Data, int64 Offset, T& Out)
	{
		if (Offset < 0 || Offset + (int64)sizeof(T) > Data.Num())
		{
			return false;
		}
		FMemory::Memcpy(&Out, Data.GetData() + Offset, sizeof(T));
		return true;
	}

	int32 ReadInt(const TArray<uint8>& Data, int64 Offset)
	{
		int32 Value = 0;
		Peek(Data, Offset, Value);
		return Value;
	}

	uint16 ReadU16(const TArray<uint8>& Data, int64 Offset)
	{
		uint16 Value = 0;
		Peek(Data, Offset, Value);
		return Value;
	}

	float ReadFloat(const TArray<uint8>& Data, int64 Offset)
	{
		float Value = 0.0f;
		Peek(Data, Offset, Value);
		return Value;
	}

	FString ReadCString(const TArray<uint8>& Data, int64 Offset)
	{
		if (Offset < 0 || Offset >= Data.Num())
		{
			return FString();
		}
		int64 End = Offset;
		while (End < Data.Num() && Data[End] != 0)
		{
			++End;
		}
		FString Out;
		FFileHelper::BufferToString(Out, Data.GetData() + Offset, (int32)(End - Offset));
		return Out;
	}
}

int32 FSourceMDLFile::GetNumTriangles() const
{
	int32 Total = 0;
	for (const FSourceMeshSection& Section : Sections)
	{
		Total += Section.Triangles.Num() / 3;
	}
	return Total;
}

bool FSourceMDLFile::ReadVtx(const FString& BasePath, TArray<uint8>& OutData, FString& OutUsedPath) const
{
	// Source looks for the best hardware-optimised mesh available for the current renderer.
	static const TCHAR* Suffixes[] = { TEXT(".dx90.vtx"), TEXT(".dx80.vtx"), TEXT(".sw.vtx"), TEXT(".vtx") };
	for (const TCHAR* Suffix : Suffixes)
	{
		const FString Path = BasePath + Suffix;
		if (FLambdaFileSystem::Get().ReadFile(Path, OutData))
		{
			OutUsedPath = Path;
			return true;
		}
	}
	return false;
}

bool FSourceMDLFile::Load(const FString& RelativeModelPath, float Scale, FString* OutError)
{
	bLoaded = false;
	Sections.Reset();

	auto Fail = [&](const FString& Msg)
	{
		if (OutError) { *OutError = Msg; }
		return false;
	};

	// "models/weapons/v_pistol.mdl" -> "models/weapons/v_pistol"
	FString BasePath = FLambdaFileSystem::NormalizeRelativePath(RelativeModelPath);
	if (BasePath.EndsWith(TEXT(".mdl"), ESearchCase::IgnoreCase))
	{
		BasePath.LeftChopInline(4);
	}

	TArray<uint8> Mdl, Vvd, Vtx;
	if (!FLambdaFileSystem::Get().ReadFile(BasePath + TEXT(".mdl"), Mdl))
	{
		return Fail(FString::Printf(TEXT("Model not found: %s.mdl"), *BasePath));
	}
	if (!FLambdaFileSystem::Get().ReadFile(BasePath + TEXT(".vvd"), Vvd))
	{
		return Fail(FString::Printf(TEXT("Vertex data not found: %s.vvd"), *BasePath));
	}
	FString VtxPath;
	if (!ReadVtx(BasePath, Vtx, VtxPath))
	{
		return Fail(FString::Printf(TEXT("No .vtx found for %s"), *BasePath));
	}

	if (ReadInt(Mdl, MDL::OFF_ID) != MDL_IDENT)
	{
		return Fail(TEXT("Not a studio model (bad IDST ident)"));
	}
	if (ReadInt(Vvd, VVD::OFF_ID) != VVD_IDENT)
	{
		return Fail(TEXT("Bad .vvd ident"));
	}

	Version = ReadInt(Mdl, MDL::OFF_VERSION);
	ModelName = ReadCString(Mdl, MDL::OFF_NAME);
	NumBones = ReadInt(Mdl, MDL::OFF_NUMBONES);

	// All three files must belong to the same model.
	const int32 Checksum = ReadInt(Mdl, MDL::OFF_CHECKSUM);
	if (ReadInt(Vvd, VVD::OFF_CHECKSUM) != Checksum || ReadInt(Vtx, VTX::OFF_CHECKSUM) != Checksum)
	{
		return Fail(TEXT("Checksum mismatch between .mdl, .vvd and .vtx (mismatched model files)"));
	}

	// ---- Materials: cdmaterials path + texture name ----
	const int32 NumTextures = ReadInt(Mdl, MDL::OFF_NUMTEXTURES);
	const int32 TextureIndex = ReadInt(Mdl, MDL::OFF_TEXTUREINDEX);
	const int32 NumCdTextures = ReadInt(Mdl, MDL::OFF_NUMCDTEXTURES);
	const int32 CdTextureIndex = ReadInt(Mdl, MDL::OFF_CDTEXTUREINDEX);

	TArray<FString> TextureNames;
	for (int32 i = 0; i < NumTextures; ++i)
	{
		const int64 Off = TextureIndex + (int64)i * MDL::SIZE_TEXTURE;
		TextureNames.Add(ReadCString(Mdl, Off + ReadInt(Mdl, Off)));
	}
	TArray<FString> CdMaterials;
	for (int32 i = 0; i < NumCdTextures; ++i)
	{
		FString Cd = ReadCString(Mdl, ReadInt(Mdl, CdTextureIndex + (int64)i * 4));
		Cd.ReplaceInline(TEXT("\\"), TEXT("/"));
		CdMaterials.Add(Cd);
	}

	// Resolves a material index to a VMT name the material library understands.
	auto ResolveMaterial = [&](int32 MaterialIndex) -> FString
	{
		if (!TextureNames.IsValidIndex(MaterialIndex))
		{
			return FString();
		}
		const FString& Tex = TextureNames[MaterialIndex];
		// Source tries each cdmaterials directory in turn; take the first that actually has the VMT.
		for (const FString& Cd : CdMaterials)
		{
			const FString Candidate = Cd / Tex;
			if (FLambdaFileSystem::Get().FileExists(FString::Printf(TEXT("materials/%s.vmt"), *Candidate)))
			{
				return Candidate;
			}
		}
		return CdMaterials.Num() > 0 ? (CdMaterials[0] / Tex) : Tex;
	};

	// ---- VVD vertex list for LOD 0, applying the fixup table (Studio_LoadVertexes) ----
	const int32 NumFixups = ReadInt(Vvd, VVD::OFF_NUMFIXUPS);
	const int32 FixupStart = ReadInt(Vvd, VVD::OFF_FIXUPTABLESTART);
	const int32 VertexDataStart = ReadInt(Vvd, VVD::OFF_VERTEXDATASTART);
	const int32 TangentDataStart = ReadInt(Vvd, VVD::OFF_TANGENTDATASTART);
	const int32 TotalVerts = (TangentDataStart - VertexDataStart) / VVD::SIZE_VERTEX;

	TArray<int32> VertexMap;	// LOD-0 index -> index into the raw vertex array
	if (NumFixups == 0)
	{
		VertexMap.Reserve(TotalVerts);
		for (int32 i = 0; i < TotalVerts; ++i)
		{
			VertexMap.Add(i);
		}
	}
	else
	{
		for (int32 i = 0; i < NumFixups; ++i)
		{
			const int64 Off = FixupStart + (int64)i * VVD::SIZE_FIXUP;
			const int32 FixupLod = ReadInt(Vvd, Off);
			const int32 SourceVertexID = ReadInt(Vvd, Off + 4);
			const int32 NumVertexes = ReadInt(Vvd, Off + 8);
			if (FixupLod >= 0)	// LOD 0 takes every fixup whose lod >= 0
			{
				for (int32 v = 0; v < NumVertexes; ++v)
				{
					VertexMap.Add(SourceVertexID + v);
				}
			}
		}
	}

	auto ReadVertex = [&](int32 LodVertexIndex, FVector& OutPos, FVector& OutNormal, FVector2D& OutUV) -> bool
	{
		if (!VertexMap.IsValidIndex(LodVertexIndex))
		{
			return false;
		}
		const int64 Off = VertexDataStart + (int64)VertexMap[LodVertexIndex] * VVD::SIZE_VERTEX;
		if (Off + VVD::SIZE_VERTEX > Vvd.Num())
		{
			return false;
		}
		const FVector3f Pos(ReadFloat(Vvd, Off + VVD::VERTEX_OFF_POS), ReadFloat(Vvd, Off + VVD::VERTEX_OFF_POS + 4), ReadFloat(Vvd, Off + VVD::VERTEX_OFF_POS + 8));
		const FVector3f Nrm(ReadFloat(Vvd, Off + VVD::VERTEX_OFF_NORMAL), ReadFloat(Vvd, Off + VVD::VERTEX_OFF_NORMAL + 4), ReadFloat(Vvd, Off + VVD::VERTEX_OFF_NORMAL + 8));
		OutPos = FSourceCoords::ToUE(Pos, Scale);
		OutNormal = FSourceCoords::ToUEDirection(Nrm);
		OutUV = FVector2D(ReadFloat(Vvd, Off + VVD::VERTEX_OFF_UV), ReadFloat(Vvd, Off + VVD::VERTEX_OFF_UV + 4));
		return true;
	};

	// ---- Walk body parts / models / LOD 0 / meshes, in lockstep between the .mdl and the .vtx ----
	const int32 NumBodyPartsMdl = ReadInt(Mdl, MDL::OFF_NUMBODYPARTS);
	const int32 BodyPartIndexMdl = ReadInt(Mdl, MDL::OFF_BODYPARTINDEX);
	const int32 NumBodyPartsVtx = ReadInt(Vtx, VTX::OFF_NUMBODYPARTS);
	const int32 BodyPartOffsetVtx = ReadInt(Vtx, VTX::OFF_BODYPARTOFFSET);

	TMap<FString, int32> SectionByMaterial;

	const int32 NumBodyParts = FMath::Min(NumBodyPartsMdl, NumBodyPartsVtx);
	for (int32 bp = 0; bp < NumBodyParts; ++bp)
	{
		const int64 VtxBp = BodyPartOffsetVtx + (int64)bp * VTX::SIZE_BODYPART;
		const int32 VtxNumModels = ReadInt(Vtx, VtxBp);
		const int32 VtxModelOffset = ReadInt(Vtx, VtxBp + 4);

		const int64 MdlBp = BodyPartIndexMdl + (int64)bp * MDL::SIZE_BODYPART;
		const int32 MdlNumModels = ReadInt(Mdl, MdlBp + 4);
		const int32 MdlModelIndex = ReadInt(Mdl, MdlBp + 12);

		const int32 NumModels = FMath::Min(VtxNumModels, MdlNumModels);
		for (int32 m = 0; m < NumModels; ++m)
		{
			const int64 VtxModel = VtxBp + VtxModelOffset + (int64)m * VTX::SIZE_MODEL;
			const int32 VtxNumLods = ReadInt(Vtx, VtxModel);
			const int32 VtxLodOffset = ReadInt(Vtx, VtxModel + 4);
			if (VtxNumLods <= 0)
			{
				continue;
			}

			const int64 MdlModel = MdlBp + MdlModelIndex + (int64)m * MDL::SIZE_MODEL;
			const int32 MdlNumMeshes = ReadInt(Mdl, MdlModel + MDL::MODEL_OFF_NUMMESHES);
			const int32 MdlMeshIndex = ReadInt(Mdl, MdlModel + MDL::MODEL_OFF_NUMMESHES + 4);
			const int32 MdlVertexIndex = ReadInt(Mdl, MdlModel + MDL::MODEL_OFF_NUMMESHES + 12);
			const int32 ModelVertexBase = MdlVertexIndex / VVD::SIZE_VERTEX;

			// LOD 0 is the highest detail.
			const int64 VtxLod = VtxModel + VtxLodOffset;
			const int32 VtxNumMeshes = ReadInt(Vtx, VtxLod);
			const int32 VtxMeshOffset = ReadInt(Vtx, VtxLod + 4);

			const int32 NumMeshes = FMath::Min(VtxNumMeshes, MdlNumMeshes);
			for (int32 me = 0; me < NumMeshes; ++me)
			{
				const int64 VtxMesh = VtxLod + VtxMeshOffset + (int64)me * VTX::SIZE_MESH;
				const int32 NumStripGroups = ReadInt(Vtx, VtxMesh);
				const int32 StripGroupOffset = ReadInt(Vtx, VtxMesh + 4);

				const int64 MdlMesh = MdlModel + MdlMeshIndex + (int64)me * MDL::SIZE_MESH;
				const int32 MaterialIndex = ReadInt(Mdl, MdlMesh);
				const int32 MeshVertexOffset = ReadInt(Mdl, MdlMesh + 12);

				// Group triangles by material so each becomes one procedural mesh section.
				const FString MaterialName = ResolveMaterial(MaterialIndex).ToLower();
				int32 SectionIndex = INDEX_NONE;
				if (const int32* Existing = SectionByMaterial.Find(MaterialName))
				{
					SectionIndex = *Existing;
				}
				else
				{
					SectionIndex = Sections.AddDefaulted();
					Sections[SectionIndex].MaterialName = MaterialName;
					SectionByMaterial.Add(MaterialName, SectionIndex);
				}
				FSourceMeshSection& Section = Sections[SectionIndex];

				for (int32 sg = 0; sg < NumStripGroups; ++sg)
				{
					const int64 StripGroup = VtxMesh + StripGroupOffset + (int64)sg * VTX::SIZE_STRIPGROUP;
					const int32 SgVertOffset = ReadInt(Vtx, StripGroup + 4);
					const int32 SgIndexOffset = ReadInt(Vtx, StripGroup + 12);
					const int32 SgNumStrips = ReadInt(Vtx, StripGroup + 16);
					const int32 SgStripOffset = ReadInt(Vtx, StripGroup + 20);

					for (int32 s = 0; s < SgNumStrips; ++s)
					{
						const int64 Strip = StripGroup + SgStripOffset + (int64)s * VTX::SIZE_STRIP;
						const int32 StripNumIndices = ReadInt(Vtx, Strip);
						const int32 StripIndexOffset = ReadInt(Vtx, Strip + 4);
						uint8 StripFlags = 0;
						Peek(Vtx, Strip + 18, StripFlags);

						// Resolves one VTX index to a final vertex in this section, adding it if new.
						auto EmitVertex = [&](int32 IndexInStrip) -> int32
						{
							const uint16 VtxIndex = ReadU16(Vtx, StripGroup + SgIndexOffset + (int64)IndexInStrip * 2);
							const uint16 OrigMeshVertID = ReadU16(Vtx, StripGroup + SgVertOffset + (int64)VtxIndex * VTX::SIZE_VERTEX + VTX::VERTEX_OFF_ORIGMESHVERTID);
							const int32 LodVertexIndex = ModelVertexBase + MeshVertexOffset + OrigMeshVertID;

							FVector Pos, Normal;
							FVector2D UV;
							if (!ReadVertex(LodVertexIndex, Pos, Normal, UV))
							{
								return INDEX_NONE;
							}
							const int32 New = Section.Vertices.Add(Pos);
							Section.Normals.Add(Normal);
							Section.UV0.Add(UV);
							Section.Colors.Add(FLinearColor::White);
							Section.Tangents.Add(FProcMeshTangent(1.0f, 0.0f, 0.0f));
							return New;
						};

						if (StripFlags & VTX::STRIP_IS_TRILIST)
						{
							for (int32 i = 0; i + 2 < StripNumIndices; i += 3)
							{
								const int32 A = EmitVertex(StripIndexOffset + i);
								const int32 B = EmitVertex(StripIndexOffset + i + 1);
								const int32 C = EmitVertex(StripIndexOffset + i + 2);
								if (A == INDEX_NONE || B == INDEX_NONE || C == INDEX_NONE)
								{
									continue;
								}
								// The Y mirror flips handedness, so reverse the winding as everywhere else.
								Section.Triangles.Add(A);
								Section.Triangles.Add(C);
								Section.Triangles.Add(B);
							}
						}
						else
						{
							// Triangle strip: every new index closes a triangle, alternating orientation.
							for (int32 i = 0; i + 2 < StripNumIndices; ++i)
							{
								const int32 A = EmitVertex(StripIndexOffset + i);
								const int32 B = EmitVertex(StripIndexOffset + i + 1);
								const int32 C = EmitVertex(StripIndexOffset + i + 2);
								if (A == INDEX_NONE || B == INDEX_NONE || C == INDEX_NONE)
								{
									continue;
								}
								if (i & 1)
								{
									Section.Triangles.Add(A);
									Section.Triangles.Add(B);
									Section.Triangles.Add(C);
								}
								else
								{
									Section.Triangles.Add(A);
									Section.Triangles.Add(C);
									Section.Triangles.Add(B);
								}
							}
						}
					}
				}
			}
		}
	}

	bLoaded = true;
	UE_LOG(LogLambdaSource, Log, TEXT("Model '%s' (v%d, %d bones): %d sections, %d tris [%s]"),
		*ModelName, Version, NumBones, Sections.Num(), GetNumTriangles(), *VtxPath);
	for (const FSourceMeshSection& Section : Sections)
	{
		UE_LOG(LogLambdaSource, Verbose, TEXT("  section '%s': %d verts, %d tris"),
			*Section.MaterialName, Section.Vertices.Num(), Section.Triangles.Num() / 3);
	}
	return true;
}
