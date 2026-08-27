#include "Formats/SourceMDLFile.h"
#include "FileSystem/LambdaFileSystem.h"
#include "Core/LambdaSourceModule.h"
#include "Core/SourceCoordinates.h"
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
		constexpr int32 OFF_NUMLOCALANIM = 180, OFF_LOCALANIMINDEX = 184;
		constexpr int32 OFF_NUMLOCALSEQ = 188, OFF_LOCALSEQINDEX = 192;
		constexpr int32 OFF_NUMINCLUDEMODELS = 336, OFF_INCLUDEMODELINDEX = 340;
		constexpr int32 OFF_NUMLOCALATTACHMENTS = 240, OFF_LOCALATTACHMENTINDEX = 244;

		constexpr int32 SIZE_TEXTURE = 64;		// mstudiotexture_t
		constexpr int32 SIZE_BODYPART = 16;		// mstudiobodyparts_t
		constexpr int32 SIZE_MODEL = 148;		// mstudiomodel_t
		constexpr int32 SIZE_MESH = 116;		// mstudiomesh_t
		// mstudiomodel_t: name[64], type, boundingradius, nummeshes, meshindex, numvertices, vertexindex, ...
		constexpr int32 MODEL_OFF_NUMMESHES = 72;

		constexpr int32 SIZE_BONE = 216;		// mstudiobone_t
		// mstudiobone_t: sznameindex, parent, bonecontroller[6], pos, quat, rot, posscale, rotscale, poseToBone, qAlignment, flags
		constexpr int32 BONE_OFF_PARENT = 4, BONE_OFF_POS = 32, BONE_OFF_QUAT = 44, BONE_OFF_ROT = 60;
		constexpr int32 BONE_OFF_POSSCALE = 72, BONE_OFF_ROTSCALE = 84, BONE_OFF_POSETOBONE = 96;
		constexpr int32 BONE_OFF_ALIGNMENT = 144, BONE_OFF_FLAGS = 160;

		constexpr int32 SIZE_ANIMDESC = 100;	// mstudioanimdesc_t
		constexpr int32 ANIM_OFF_NAMEINDEX = 4, ANIM_OFF_FPS = 8, ANIM_OFF_FLAGS = 12, ANIM_OFF_NUMFRAMES = 16;
		constexpr int32 ANIM_OFF_ANIMBLOCK = 52, ANIM_OFF_ANIMINDEX = 56;
		constexpr int32 ANIM_OFF_SECTIONINDEX = 80, ANIM_OFF_SECTIONFRAMES = 84;

		constexpr int32 SIZE_SEQDESC = 212;		// mstudioseqdesc_t
		constexpr int32 SEQ_OFF_LABELINDEX = 4, SEQ_OFF_ACTIVITYNAMEINDEX = 8, SEQ_OFF_FLAGS = 12;
		constexpr int32 SEQ_OFF_ACTWEIGHT = 20, SEQ_OFF_NUMEVENTS = 24, SEQ_OFF_EVENTINDEX = 28;
		constexpr int32 SEQ_OFF_NUMBLENDS = 56, SEQ_OFF_ANIMINDEXINDEX = 60;
		constexpr int32 SEQ_OFF_GROUPSIZE0 = 68, SEQ_OFF_GROUPSIZE1 = 72;
		constexpr int32 SEQ_OFF_PARAMINDEX0 = 76, SEQ_OFF_PARAMINDEX1 = 80;
		constexpr int32 SEQ_OFF_PARAMSTART0 = 84, SEQ_OFF_PARAMSTART1 = 88;
		constexpr int32 SEQ_OFF_PARAMEND0 = 92, SEQ_OFF_PARAMEND1 = 96;
		constexpr int32 OFF_NUMLOCALPOSEPARAM = 300, OFF_LOCALPOSEPARAMINDEX = 304;
		constexpr int32 SIZE_POSEPARAMDESC = 20;
		constexpr int32 SEQ_OFF_FADEINTIME = 104, SEQ_OFF_FADEOUTTIME = 108;
		constexpr int32 SEQ_OFF_NUMAUTOLAYERS = 148, SEQ_OFF_AUTOLAYERINDEX = 152, SEQ_OFF_WEIGHTLISTINDEX = 156;
		constexpr int32 SIZE_AUTOLAYER = 24;	// mstudioautolayer_t: short iSequence, short iPose, int flags, float start, peak, tail, end

		constexpr int32 OFF_NUMHITBOXSETS = 172, OFF_HITBOXSETINDEX = 176;
		constexpr int32 SIZE_HITBOXSET = 12;	// mstudiohitboxset_t: sznameindex, numhitboxes, hitboxindex
		constexpr int32 SIZE_BBOX = 68;			// mstudiobbox_t: bone, group, bbmin, bbmax, szhitboxnameindex, unused[8]

		// mstudioevent_t: cycle, event, type, options[64], szeventindex - 4+4+4+64+4 = 80 bytes.
		constexpr int32 SIZE_EVENT = 80;
		constexpr int32 EVENT_OFF_EVENT = 4, EVENT_OFF_OPTIONS = 12, EVENT_OFF_NAMEINDEX = 76;

		constexpr int32 OFF_HULLMIN = 104, OFF_HULLMAX = 116;
		constexpr int32 OFF_SURFACEPROPINDEX = 308;
		// studiohdr_t::KeyValueText - the model's own keyvalues, which is where prop_data lives.
		constexpr int32 OFF_KEYVALUEINDEX = 312, OFF_KEYVALUESIZE = 316;
		constexpr int32 OFF_SZANIMBLOCKNAMEINDEX = 348, OFF_NUMANIMBLOCKS = 352, OFF_ANIMBLOCKINDEX = 356;
		constexpr int32 SIZE_ANIMBLOCK = 8;		// mstudioanimblock_t: datastart, dataend
		constexpr int32 SIZE_ANIMSECTION = 8;	// mstudioanimsections_t: animblock, animindex
		constexpr int32 ANIM_OFF_NUMMOVEMENTS = 20, ANIM_OFF_MOVEMENTINDEX = 24;
		constexpr int32 SIZE_MOVEMENT = 44;		// mstudiomovement_t: endframe, motionflags, v0, v1, angle, vector, position
		constexpr int32 MOVEMENT_OFF_POSITION = 32;

		constexpr int32 SIZE_ATTACHMENT = 92;	// mstudioattachment_t: sznameindex, flags, localbone, local(48), unused[8]
		constexpr int32 ATTACH_OFF_BONE = 8, ATTACH_OFF_LOCAL = 12;
	}

	/** studio.h per-bone animation flags (mstudio_rle_anim_t::flags). FLAG_-prefixed: <winnt.h> defines DELTA. */
	namespace STUDIOANIM
	{
		constexpr uint8 FLAG_RAWPOS = 0x01;		// Vector48
		constexpr uint8 FLAG_RAWROT = 0x02;		// Quaternion48
		constexpr uint8 FLAG_ANIMPOS = 0x04;		// mstudioanim_valueptr_t
		constexpr uint8 FLAG_ANIMROT = 0x08;		// mstudioanim_valueptr_t
		constexpr uint8 FLAG_DELTA = 0x10;
		constexpr uint8 FLAG_RAWROT2 = 0x20;		// Quaternion64

		constexpr int32 SIZE_RLE_HEADER = 4;	// bone, flags, nextoffset
		constexpr int32 SIZE_QUAT48 = 6;
		constexpr int32 SIZE_QUAT64 = 8;
		constexpr int32 SIZE_VALUEPTR = 6;		// short offset[3]
	}

	/** studio.h sequence / animation / autolayer flags (prefixed: <winnt.h> defines DELTA). */
	namespace STUDIO
	{
		constexpr int32 SEQ_DELTA = 0x0004;		// this sequence "adds" to whatever is under it
		constexpr int32 SEQ_POST = 0x0010;		// ...and is applied after (QuaternionMA) rather than before
		constexpr int32 ANIM_ALLZEROS = 0x0020;
		constexpr int32 AL_POST = 0x0010;
		constexpr int32 AL_SPLINE = 0x0040;
		constexpr int32 AL_XFADE = 0x0080;
		constexpr int32 AL_NOBLEND = 0x0200;
		constexpr int32 AL_LOCAL = 0x1000;
		constexpr int32 AL_POSE = 0x4000;
	}

	namespace VVD
	{
		constexpr int32 OFF_ID = 0, OFF_VERSION = 4, OFF_CHECKSUM = 8, OFF_NUMLODS = 12;
		constexpr int32 OFF_NUMLODVERTEXES = 16;	// int[8]
		constexpr int32 OFF_NUMFIXUPS = 48, OFF_FIXUPTABLESTART = 52, OFF_VERTEXDATASTART = 56, OFF_TANGENTDATASTART = 60;
		constexpr int32 SIZE_VERTEX = 48;			// mstudiovertex_t: boneweights(16) pos(12) normal(12) uv(8)
		constexpr int32 VERTEX_OFF_POS = 16, VERTEX_OFF_NORMAL = 28, VERTEX_OFF_UV = 40;
		// mstudioboneweight_t sits at the front of mstudiovertex_t: float weight[3], char bone[3], byte numbones.
		constexpr int32 VERTEX_OFF_WEIGHTS = 0, VERTEX_OFF_BONES = 12, VERTEX_OFF_NUMBONES = 15;
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

namespace
{
	// ---- Compressed value formats (mathlib/compressed_vector.h) ----

	/** Source's float16: 5-bit exponent, 10-bit mantissa, with its own denormal and "infinity clamps to max" rules. */
	float ReadFloat16(uint16 Bits)
	{
		const uint32 Sign = (Bits >> 15) & 1;
		const uint32 Exp = (Bits >> 10) & 0x1F;
		const uint32 Mantissa = Bits & 0x3FF;

		float Value;
		if (Exp == 0)
		{
			Value = (Mantissa / 1024.0f) * FMath::Pow(2.0f, -14.0f);
		}
		else if (Exp == 31)
		{
			Value = 65504.0f;
		}
		else
		{
			Value = (1.0f + Mantissa / 1024.0f) * FMath::Pow(2.0f, (float)Exp - 15.0f);
		}
		return Sign ? -Value : Value;
	}

	FVector3f ReadVector48(const TArray<uint8>& Data, int64 Offset)
	{
		return FVector3f(ReadFloat16(ReadU16(Data, Offset)),
			ReadFloat16(ReadU16(Data, Offset + 2)),
			ReadFloat16(ReadU16(Data, Offset + 4)));
	}

	/** Quaternion48: x:16, y:16, z:15, wneg:1. w is recovered from the unit-length constraint. */
	FQuat4f ReadQuat48(const TArray<uint8>& Data, int64 Offset)
	{
		uint64 Raw = 0;
		for (int32 i = 0; i < 6; ++i)
		{
			uint8 Byte = 0;
			Peek(Data, Offset + i, Byte);
			Raw |= (uint64)Byte << (8 * i);
		}
		const float X = ((int32)(Raw & 0xFFFF) - 32768) * (1.0f / 32768.5f);
		const float Y = ((int32)((Raw >> 16) & 0xFFFF) - 32768) * (1.0f / 32768.5f);
		const float Z = ((int32)((Raw >> 32) & 0x7FFF) - 16384) * (1.0f / 16384.5f);
		float W = FMath::Sqrt(FMath::Max(0.0f, 1.0f - X * X - Y * Y - Z * Z));
		if ((Raw >> 47) & 1)
		{
			W = -W;
		}
		return FQuat4f(X, Y, Z, W);
	}

	/** Quaternion64: x:21, y:21, z:21, wneg:1. */
	FQuat4f ReadQuat64(const TArray<uint8>& Data, int64 Offset)
	{
		uint64 Raw = 0;
		for (int32 i = 0; i < 8; ++i)
		{
			uint8 Byte = 0;
			Peek(Data, Offset + i, Byte);
			Raw |= (uint64)Byte << (8 * i);
		}
		const float X = ((int32)(Raw & 0x1FFFFF) - 1048576) * (1.0f / 1048576.5f);
		const float Y = ((int32)((Raw >> 21) & 0x1FFFFF) - 1048576) * (1.0f / 1048576.5f);
		const float Z = ((int32)((Raw >> 42) & 0x1FFFFF) - 1048576) * (1.0f / 1048576.5f);
		float W = FMath::Sqrt(FMath::Max(0.0f, 1.0f - X * X - Y * Y - Z * Z));
		if ((Raw >> 63) & 1)
		{
			W = -W;
		}
		return FQuat4f(X, Y, Z, W);
	}

	int16 ReadI16(const TArray<uint8>& Data, int64 Offset)
	{
		int16 Value = 0;
		Peek(Data, Offset, Value);
		return Value;
	}

	FVector3f ReadVec3(const TArray<uint8>& Data, int64 Offset)
	{
		return FVector3f(ReadFloat(Data, Offset), ReadFloat(Data, Offset + 4), ReadFloat(Data, Offset + 8));
	}

	FQuat4f ReadQuat(const TArray<uint8>& Data, int64 Offset)
	{
		return FQuat4f(ReadFloat(Data, Offset), ReadFloat(Data, Offset + 4),
			ReadFloat(Data, Offset + 8), ReadFloat(Data, Offset + 12));
	}

	FSourceMatrix3x4 ReadMatrix3x4(const TArray<uint8>& Data, int64 Offset)
	{
		FSourceMatrix3x4 Out;
		for (int32 R = 0; R < 3; ++R)
		{
			for (int32 C = 0; C < 4; ++C)
			{
				Out.M[R][C] = ReadFloat(Data, Offset + (R * 4 + C) * 4);
			}
		}
		return Out;
	}

	/** AngleQuaternion(RadianEuler) from mathlib - Source composes its Euler angles in X, Y, Z order. */
	FQuat4f AngleQuaternion(const FVector3f& Angles)
	{
		const float SR = FMath::Sin(Angles.X * 0.5f), CR = FMath::Cos(Angles.X * 0.5f);
		const float SP = FMath::Sin(Angles.Y * 0.5f), CP = FMath::Cos(Angles.Y * 0.5f);
		const float SY = FMath::Sin(Angles.Z * 0.5f), CY = FMath::Cos(Angles.Z * 0.5f);
		return FQuat4f(
			SR * CP * CY - CR * SP * SY,
			CR * SP * CY + SR * CP * SY,
			CR * CP * SY - SR * SP * CY,
			CR * CP * CY + SR * SP * SY);
	}

	/**
	 * ExtractAnimValue (bonesetup/bone_decode.cpp), single-value form. The track is a run-length list of
	 * mstudioanimvalue_t: a {valid, total} header followed by `valid` shorts. Frames past `valid` repeat the last
	 * value, and runs are walked until the one containing the frame is found.
	 */
	float ExtractAnimValue(const TArray<uint8>& Data, int64 RunOffset, int32 Frame, float Scale)
	{
		if (RunOffset <= 0)
		{
			return 0.0f;
		}
		int64 P = RunOffset;
		int32 K = Frame;

		// Guard the walk: a corrupt track must not spin forever or read outside the file.
		for (int32 Guard = 0; Guard < 1024; ++Guard)
		{
			uint8 Valid = 0, Total = 0;
			if (!Peek(Data, P, Valid) || !Peek(Data, P + 1, Total) || Total == 0)
			{
				return 0.0f;
			}
			if (Total > K)
			{
				// v1 = panimvalue[k+1] when the frame has its own value, else the last valid one.
				const int32 Index = (Valid > K) ? (K + 1) : Valid;
				return ReadI16(Data, P + 2 * Index) * Scale;
			}
			K -= Total;
			P += 2 * ((int64)Valid + 1);
		}
		return 0.0f;
	}
}

// ---- FSourceMatrix3x4 ----

FSourceMatrix3x4 FSourceMatrix3x4::FromQuatPos(const FQuat4f& Q, const FVector3f& P)
{
	// QuaternionMatrix from mathlib.
	FSourceMatrix3x4 Out;
	Out.M[0][0] = 1.0f - 2.0f * Q.Y * Q.Y - 2.0f * Q.Z * Q.Z;
	Out.M[1][0] = 2.0f * Q.X * Q.Y + 2.0f * Q.W * Q.Z;
	Out.M[2][0] = 2.0f * Q.X * Q.Z - 2.0f * Q.W * Q.Y;

	Out.M[0][1] = 2.0f * Q.X * Q.Y - 2.0f * Q.W * Q.Z;
	Out.M[1][1] = 1.0f - 2.0f * Q.X * Q.X - 2.0f * Q.Z * Q.Z;
	Out.M[2][1] = 2.0f * Q.Y * Q.Z + 2.0f * Q.W * Q.X;

	Out.M[0][2] = 2.0f * Q.X * Q.Z + 2.0f * Q.W * Q.Y;
	Out.M[1][2] = 2.0f * Q.Y * Q.Z - 2.0f * Q.W * Q.X;
	Out.M[2][2] = 1.0f - 2.0f * Q.X * Q.X - 2.0f * Q.Y * Q.Y;

	Out.M[0][3] = P.X;
	Out.M[1][3] = P.Y;
	Out.M[2][3] = P.Z;
	return Out;
}

FVector3f FSourceMatrix3x4::TransformPosition(const FVector3f& V) const
{
	return FVector3f(
		M[0][0] * V.X + M[0][1] * V.Y + M[0][2] * V.Z + M[0][3],
		M[1][0] * V.X + M[1][1] * V.Y + M[1][2] * V.Z + M[1][3],
		M[2][0] * V.X + M[2][1] * V.Y + M[2][2] * V.Z + M[2][3]);
}

FVector3f FSourceMatrix3x4::TransformVector(const FVector3f& V) const
{
	return FVector3f(
		M[0][0] * V.X + M[0][1] * V.Y + M[0][2] * V.Z,
		M[1][0] * V.X + M[1][1] * V.Y + M[1][2] * V.Z,
		M[2][0] * V.X + M[2][1] * V.Y + M[2][2] * V.Z);
}

FTransform FSourceMatrix3x4::ToUETransform(float Scale) const
{
	const FVector X = FSourceCoords::ToUEDirection(GetForward());
	const FVector Y = -FSourceCoords::ToUEDirection(GetLeft());
	const FVector Z = FSourceCoords::ToUEDirection(GetUp());
	const FVector Origin = FSourceCoords::ToUE(GetOrigin(), Scale);
	return FTransform(FMatrix(X, Y, Z, Origin));
}

FSourceMatrix3x4 FSourceMatrix3x4::FromUETransform(const FTransform& T, float Scale)
{
	const FMatrix M = T.ToMatrixNoScale();
	const FVector X = M.GetUnitAxis(EAxis::X);
	const FVector Y = M.GetUnitAxis(EAxis::Y);
	const FVector Z = M.GetUnitAxis(EAxis::Z);
	const FVector3f F((float)X.X, (float)-X.Y, (float)X.Z);
	const FVector3f L((float)-Y.X, (float)Y.Y, (float)-Y.Z);
	const FVector3f U((float)Z.X, (float)-Z.Y, (float)Z.Z);
	const FVector3f O = FSourceCoords::ToSource(T.GetLocation(), Scale);
	FSourceMatrix3x4 Out;
	for (int32 r = 0; r < 3; ++r)
	{
		Out.M[r][0] = F[r];
		Out.M[r][1] = L[r];
		Out.M[r][2] = U[r];
		Out.M[r][3] = O[r];
	}
	return Out;
}

FSourceMatrix3x4 FSourceMatrix3x4::Concat(const FSourceMatrix3x4& Other) const
{
	// ConcatTransforms: out = this * Other.
	FSourceMatrix3x4 Out;
	for (int32 R = 0; R < 3; ++R)
	{
		for (int32 C = 0; C < 3; ++C)
		{
			Out.M[R][C] = M[R][0] * Other.M[0][C] + M[R][1] * Other.M[1][C] + M[R][2] * Other.M[2][C];
		}
		Out.M[R][3] = M[R][0] * Other.M[0][3] + M[R][1] * Other.M[1][3] + M[R][2] * Other.M[2][3] + M[R][3];
	}
	return Out;
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
	SkinVertices.Reset();
	Bones.Reset();
	AnimDescs.Reset();
	Sequences.Reset();
	Attachments.Reset();
	MdlData.Reset();
	AniData.Reset();
	AnimBlocks.Reset();
	AnimBlockName.Reset();
	IncludeGroups.Reset();
	NumLocalSequences = 0;
	PoseParams.Reset();
	SurfaceProp.Reset();
	VvdData.Reset();
	VtxData.Reset();
	Hitboxes.Reset();
	BodyPartNames.Reset();
	BodyPartModelCounts.Reset();
	BodygroupValues.Reset();
	UnitScale = Scale;

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
	FString VtxPathUsed;
	if (!ReadVtx(BasePath, Vtx, VtxPathUsed))
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

	// All three files must belong to the same model.
	const int32 Checksum = ReadInt(Mdl, MDL::OFF_CHECKSUM);
	if (ReadInt(Vvd, VVD::OFF_CHECKSUM) != Checksum || ReadInt(Vtx, VTX::OFF_CHECKSUM) != Checksum)
	{
		return Fail(TEXT("Checksum mismatch between .mdl, .vvd and .vtx (mismatched model files)"));
	}

	ReadBones(Mdl);
	ReadPoseParams(Mdl);
	ReadSequences(Mdl);
	ReadAttachments(Mdl);

	SurfaceProp = ReadCString(Mdl, ReadInt(Mdl, MDL::OFF_SURFACEPROPINDEX));

	// The keyvalue text is a plain KeyValues block: "mdlkeyvalue { prop_data { ... } }".
	const int32 KeyValueIndex = ReadInt(Mdl, MDL::OFF_KEYVALUEINDEX);
	const int32 KeyValueSize = ReadInt(Mdl, MDL::OFF_KEYVALUESIZE);
	if (KeyValueSize > 0 && KeyValueIndex > 0 && KeyValueIndex + KeyValueSize <= Mdl.Num())
	{
		KeyValueText = ReadCString(Mdl, KeyValueIndex);
	}
	HullMin = ReadVec3(Mdl, MDL::OFF_HULLMIN);
	HullMax = ReadVec3(Mdl, MDL::OFF_HULLMAX);

	// External animation blocks. studiomdl writes character animation to "<model>.ani" and leaves only the block
	// table (and any block-0 animations) in the .mdl; without the file every sequence would fall back to the bind
	// pose, which is what we showed for view models before this existed.
	const int32 NumAnimBlocks = ReadInt(Mdl, MDL::OFF_NUMANIMBLOCKS);
	const int32 AnimBlockIndex = ReadInt(Mdl, MDL::OFF_ANIMBLOCKINDEX);
	for (int32 i = 0; i < NumAnimBlocks; ++i)
	{
		const int64 Off = AnimBlockIndex + (int64)i * MDL::SIZE_ANIMBLOCK;
		AnimBlocks.Emplace(ReadInt(Mdl, Off), ReadInt(Mdl, Off + 4));
	}
	if (NumAnimBlocks > 1)
	{
		AnimBlockName = ReadCString(Mdl, ReadInt(Mdl, MDL::OFF_SZANIMBLOCKNAMEINDEX));
		AnimBlockName.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (!AnimBlockName.IsEmpty() && !FLambdaFileSystem::Get().ReadFile(AnimBlockName, AniData))
		{
			UE_LOG(LogLambdaSource, Warning, TEXT("Model '%s': animation file '%s' not found; sequences in external blocks will show the bind pose"),
				*BasePath, *AnimBlockName);
		}
	}

	MdlData = MoveTemp(Mdl);
	VvdData = MoveTemp(Vvd);
	VtxData = MoveTemp(Vtx);
	VtxPath = VtxPathUsed;
	ReadHitboxes(MdlData);

	// $includemodel, after everything local: the borrowed sequences append to the local ones.
	NumLocalSequences = Sequences.Num();
	{
		TArray<FString> Visited;
		Visited.Add(BasePath.ToLower());
		LoadIncludeModels(MdlData, Visited);
	}

	// Body parts and their model counts; every bodygroup starts at model 0 (Source's default body).
	{
		const int32 NumBodyPartsMdl = ReadInt(MdlData, MDL::OFF_NUMBODYPARTS);
		const int32 BodyPartIndexMdl = ReadInt(MdlData, MDL::OFF_BODYPARTINDEX);
		for (int32 bp = 0; bp < NumBodyPartsMdl; ++bp)
		{
			const int64 MdlBp = BodyPartIndexMdl + (int64)bp * MDL::SIZE_BODYPART;
			BodyPartNames.Add(ReadCString(MdlData, MdlBp + ReadInt(MdlData, MdlBp)));
			BodyPartModelCounts.Add(ReadInt(MdlData, MdlBp + 4));
			BodygroupValues.Add(0);
		}
	}
	BuildSections();
	bLoaded = true;
	UE_LOG(LogLambdaSource, Log, TEXT("Model '%s' (v%d): %d bones, %d sequences, %d attachments, %d sections, %d tris [%s]"),
		*ModelName, Version, Bones.Num(), Sequences.Num(), Attachments.Num(), Sections.Num(), GetNumTriangles(), *VtxPath);
	for (const FSourceMeshSection& Section : Sections)
	{
		UE_LOG(LogLambdaSource, Verbose, TEXT("  section '%s': %d verts, %d tris"),
			*Section.MaterialName, Section.Vertices.Num(), Section.Triangles.Num() / 3);
	}
	return true;
}

void FSourceMDLFile::BuildSections()
{
	Sections.Reset();
	SkinVertices.Reset();
	const TArray<uint8>& Mdl = MdlData;
	const TArray<uint8>& Vvd = VvdData;
	const TArray<uint8>& Vtx = VtxData;
	const float Scale = UnitScale;
	if (Mdl.Num() == 0 || Vvd.Num() == 0 || Vtx.Num() == 0)
	{
		return;
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

	auto ReadVertex = [&](int32 LodVertexIndex, FVector& OutPos, FVector& OutNormal, FVector2D& OutUV,
		FSourceSkinVertex& OutSkin) -> bool
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
		const FVector3f Pos = ReadVec3(Vvd, Off + VVD::VERTEX_OFF_POS);
		const FVector3f Nrm = ReadVec3(Vvd, Off + VVD::VERTEX_OFF_NORMAL);
		OutPos = FSourceCoords::ToUE(Pos, Scale);
		OutNormal = FSourceCoords::ToUEDirection(Nrm);
		OutUV = FVector2D(ReadFloat(Vvd, Off + VVD::VERTEX_OFF_UV), ReadFloat(Vvd, Off + VVD::VERTEX_OFF_UV + 4));

		// Keep the untransformed vertex and its mstudioboneweight_t so ApplyPose can re-skin it later.
		OutSkin.Position = Pos;
		OutSkin.Normal = Nrm;
		uint8 NumBoneWeights = 0;
		Peek(Vvd, Off + VVD::VERTEX_OFF_NUMBONES, NumBoneWeights);
		OutSkin.NumBones = FMath::Clamp<uint8>(NumBoneWeights, 0, 3);
		for (int32 b = 0; b < OutSkin.NumBones; ++b)
		{
			uint8 BoneId = 0;
			Peek(Vvd, Off + VVD::VERTEX_OFF_BONES + b, BoneId);
			OutSkin.Bones[b] = BoneId;
			OutSkin.Weights[b] = ReadFloat(Vvd, Off + VVD::VERTEX_OFF_WEIGHTS + b * 4);
		}
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
			// SetBodygroup: only the chosen model of each body part is built (Source's default body is 0 for all).
			if (m != GetBodygroup(bp))
			{
				continue;
			}
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
					SkinVertices.AddDefaulted();
					Sections[SectionIndex].MaterialName = MaterialName;
					SectionByMaterial.Add(MaterialName, SectionIndex);
				}
				FSourceMeshSection& Section = Sections[SectionIndex];
				TArray<FSourceSkinVertex>& SectionSkin = SkinVertices[SectionIndex];

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
							FSourceSkinVertex Skin;
							if (!ReadVertex(LodVertexIndex, Pos, Normal, UV, Skin))
							{
								return INDEX_NONE;
							}
							const int32 New = Section.Vertices.Add(Pos);
							Section.Normals.Add(Normal);
							Section.UV0.Add(UV);
							Section.Colors.Add(FLinearColor::White);
							Section.Tangents.Add(FProcMeshTangent(1.0f, 0.0f, 0.0f));
							SectionSkin.Add(Skin);
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

}

bool FSourceMDLFile::SetBodygroup(int32 BodyPart, int32 Value)
{
	if (!BodygroupValues.IsValidIndex(BodyPart))
	{
		return false;
	}
	Value = FMath::Clamp(Value, 0, FMath::Max(0, BodyPartModelCounts[BodyPart] - 1));
	if (BodygroupValues[BodyPart] == Value)
	{
		return true;
	}
	BodygroupValues[BodyPart] = Value;
	BuildSections();
	return true;
}

int32 FSourceMDLFile::FindBodyPart(const FString& Name) const
{
	for (int32 i = 0; i < BodyPartNames.Num(); ++i)
	{
		if (BodyPartNames[i].Equals(Name, ESearchCase::IgnoreCase))
		{
			return i;
		}
	}
	return INDEX_NONE;
}

void FSourceMDLFile::ReadHitboxes(const TArray<uint8>& Mdl)
{
	// The first hitbox set is the default one (models with several switch them for gameplay states).
	const int32 NumSets = ReadInt(Mdl, MDL::OFF_NUMHITBOXSETS);
	const int32 SetIndex = ReadInt(Mdl, MDL::OFF_HITBOXSETINDEX);
	if (NumSets <= 0)
	{
		return;
	}
	const int64 Set = SetIndex;
	const int32 NumHitboxes = ReadInt(Mdl, Set + 4);
	const int32 HitboxIndex = ReadInt(Mdl, Set + 8);
	for (int32 h = 0; h < NumHitboxes; ++h)
	{
		const int64 Off = Set + HitboxIndex + (int64)h * MDL::SIZE_BBOX;
		FSourceStudioHitbox& Box = Hitboxes.AddDefaulted_GetRef();
		Box.Bone = ReadInt(Mdl, Off);
		Box.Group = ReadInt(Mdl, Off + 4);
		Box.Min = ReadVec3(Mdl, Off + 8);
		Box.Max = ReadVec3(Mdl, Off + 20);
		const int32 NameIndex = ReadInt(Mdl, Off + 32);
		Box.Name = NameIndex != 0 ? ReadCString(Mdl, Off + NameIndex) : FString();
	}
}

// ---- Bones, sequences, attachments ----

bool FSourceMDLFile::LoadAnimationLibrary(const FString& RelativeModelPath, float Scale, TArray<FString>& Visited)
{
	UnitScale = Scale;
	FString BasePath = FLambdaFileSystem::NormalizeRelativePath(RelativeModelPath);
	if (BasePath.EndsWith(TEXT(".mdl"), ESearchCase::IgnoreCase))
	{
		BasePath.LeftChopInline(4);
	}
	TArray<uint8> Mdl;
	if (!FLambdaFileSystem::Get().ReadFile(BasePath + TEXT(".mdl"), Mdl) || ReadInt(Mdl, MDL::OFF_ID) != MDL_IDENT)
	{
		return false;
	}
	Version = ReadInt(Mdl, MDL::OFF_VERSION);
	ModelName = ReadCString(Mdl, MDL::OFF_NAME);

	ReadBones(Mdl);
	ReadPoseParams(Mdl);
	ReadSequences(Mdl);

	// The libraries keep long animations in a .ani beside the .mdl, the same as any model.
	const int32 NumAnimBlocks = ReadInt(Mdl, MDL::OFF_NUMANIMBLOCKS);
	const int32 AnimBlockIndex = ReadInt(Mdl, MDL::OFF_ANIMBLOCKINDEX);
	for (int32 i = 0; i < NumAnimBlocks; ++i)
	{
		const int64 Off = AnimBlockIndex + (int64)i * MDL::SIZE_ANIMBLOCK;
		AnimBlocks.Emplace(ReadInt(Mdl, Off), ReadInt(Mdl, Off + 4));
	}
	if (NumAnimBlocks > 1)
	{
		AnimBlockName = ReadCString(Mdl, ReadInt(Mdl, MDL::OFF_SZANIMBLOCKNAMEINDEX));
		AnimBlockName.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (!AnimBlockName.IsEmpty() && !FLambdaFileSystem::Get().ReadFile(AnimBlockName, AniData))
		{
			UE_LOG(LogLambdaSource, Warning, TEXT("Animation library '%s': .ani '%s' not found"), *BasePath, *AnimBlockName);
		}
	}

	MdlData = MoveTemp(Mdl);
	NumLocalSequences = Sequences.Num();
	LoadIncludeModels(MdlData, Visited);
	return true;
}

void FSourceMDLFile::LoadIncludeModels(const TArray<uint8>& Mdl, TArray<FString>& Visited)
{
	const int32 Num = ReadInt(Mdl, MDL::OFF_NUMINCLUDEMODELS);
	const int32 Index = ReadInt(Mdl, MDL::OFF_INCLUDEMODELINDEX);
	if (Num <= 0 || Num > 32 || Index <= 0)
	{
		return;
	}
	for (int32 i = 0; i < Num; ++i)
	{
		// mstudiomodelgroup_t: a label and a file name, both relative to the struct.
		const int64 Off = Index + (int64)i * 8;
		const int32 NameIndex = ReadInt(Mdl, Off + 4);
		FString Name = NameIndex > 0 ? ReadCString(Mdl, Off + NameIndex) : FString();
		Name.ReplaceInline(TEXT("\\"), TEXT("/"));	// male_shared names "models/player\male_anims.mdl"
		if (Name.IsEmpty())
		{
			continue;
		}
		if (!Name.StartsWith(TEXT("models/"), ESearchCase::IgnoreCase))
		{
			Name = TEXT("models/") + Name;
		}
		FString Key = Name.ToLower();
		Key.RemoveFromEnd(TEXT(".mdl"));
		if (Visited.Contains(Key))
		{
			continue;	// the libraries include each other; each is read once
		}
		Visited.Add(Key);

		TSharedPtr<FSourceMDLFile> Library = MakeShared<FSourceMDLFile>();
		if (!Library->LoadAnimationLibrary(Name, UnitScale, Visited))
		{
			UE_LOG(LogLambdaSource, Warning, TEXT("Model '%s': $includemodel '%s' not found"), *ModelName, *Name);
			continue;
		}
		MergeInclude(Library);
	}
}

int32 FSourceMDLFile::FindOrAddIncludeGroup(const TSharedPtr<FSourceMDLFile>& File)
{
	for (int32 i = 0; i < IncludeGroups.Num(); ++i)
	{
		if (IncludeGroups[i].File == File)
		{
			return i;
		}
	}
	// The map from the library's bones to ours, by name - virtualmodel_t's masterbone.
	TMap<FString, int32> ByName;
	for (int32 b = 0; b < Bones.Num(); ++b)
	{
		ByName.Add(Bones[b].Name.ToLower(), b);
	}
	FIncludeGroup Group;
	Group.File = File;
	Group.BoneMap.SetNum(File->Bones.Num());
	Group.HostToInclude.Init(INDEX_NONE, Bones.Num());
	int32 Mapped = 0;
	for (int32 b = 0; b < File->Bones.Num(); ++b)
	{
		const int32* Found = ByName.Find(File->Bones[b].Name.ToLower());
		Group.BoneMap[b] = Found ? *Found : INDEX_NONE;
		if (Found)
		{
			Group.HostToInclude[*Found] = b;
			++Mapped;
		}
	}
	UE_LOG(LogLambdaSource, Verbose, TEXT("Model '%s': include group '%s', %d of %d bones map"),
		*ModelName, *File->ModelName, Mapped, File->Bones.Num());
	IncludeGroups.Add(MoveTemp(Group));
	return IncludeGroups.Num() - 1;
}

void FSourceMDLFile::MergeInclude(const TSharedPtr<FSourceMDLFile>& Child)
{
	// The child arrives already flattened - its own includes were merged as it loaded - so each of its anim
	// descs names the file its frames really live in, and every group becomes a group of ours.
	const int32 SeqBase = Sequences.Num();
	const int32 AnimBase = AnimDescs.Num();

	TArray<int32> GroupRemap;
	GroupRemap.Add(FindOrAddIncludeGroup(Child) + 1);
	for (const FIncludeGroup& G : Child->IncludeGroups)
	{
		GroupRemap.Add(FindOrAddIncludeGroup(G.File) + 1);
	}

	for (const FSourceStudioAnimDesc& A : Child->AnimDescs)
	{
		FSourceStudioAnimDesc Copy = A;
		Copy.Group = GroupRemap.IsValidIndex(A.Group) ? GroupRemap[A.Group] : GroupRemap[0];
		AnimDescs.Add(MoveTemp(Copy));
	}
	for (const FSourceStudioSequence& S : Child->Sequences)
	{
		FSourceStudioSequence Copy = S;
		if (Copy.AnimDescIndex != INDEX_NONE)
		{
			Copy.AnimDescIndex += AnimBase;
		}
		for (int32& Cell : Copy.BlendAnimDescs)
		{
			if (Cell != INDEX_NONE)
			{
				Cell += AnimBase;
			}
		}
		// The sequence's pose parameters are the library's; the host adopts any it lacks, by name, so
		// "aim_pitch" is one control however many libraries mention it.
		for (int32 Axis = 0; Axis < 2; ++Axis)
		{
			if (Copy.ParamIndex[Axis] >= 0 && Child->PoseParams.IsValidIndex(Copy.ParamIndex[Axis]))
			{
				Copy.ParamIndex[Axis] = FindOrAddPoseParam(Child->PoseParams[Copy.ParamIndex[Axis]]);
			}
		}
		for (FSourceStudioAutoLayer& Layer : Copy.AutoLayers)
		{
			if (Layer.Pose >= 0 && Child->PoseParams.IsValidIndex(Layer.Pose))
			{
				Layer.Pose = FindOrAddPoseParam(Child->PoseParams[Layer.Pose]);
			}
		}
		for (FSourceStudioAutoLayer& Layer : Copy.AutoLayers)
		{
			if (Layer.Sequence != INDEX_NONE)
			{
				Layer.Sequence += SeqBase;
			}
		}
		// The $weightlist stays in the bone order it was written in; the sequence just remembers whose that is,
		// and SequenceBoneWeight resolves it when the pose is built. Rewriting it here would push it through
		// whatever model happens to sit between - and male_shared, the stub every human includes, has no bones
		// to map onto at all.
		Copy.WeightGroup = GroupRemap.IsValidIndex(S.WeightGroup) ? GroupRemap[S.WeightGroup] : GroupRemap[0];
		Sequences.Add(MoveTemp(Copy));
	}
}

void FSourceMDLFile::ReadPoseParams(const TArray<uint8>& Mdl)
{
	const int32 Num = ReadInt(Mdl, MDL::OFF_NUMLOCALPOSEPARAM);
	const int32 Index = ReadInt(Mdl, MDL::OFF_LOCALPOSEPARAMINDEX);
	for (int32 i = 0; i < Num && i < 64; ++i)
	{
		const int64 Off = Index + (int64)i * MDL::SIZE_POSEPARAMDESC;
		FSourcePoseParam& Param = PoseParams.AddDefaulted_GetRef();
		Param.Name = ReadCString(Mdl, Off + ReadInt(Mdl, Off));
		Param.Start = ReadFloat(Mdl, Off + 8);
		Param.End = ReadFloat(Mdl, Off + 12);
		Param.Loop = ReadFloat(Mdl, Off + 16);
	}
}

int32 FSourceMDLFile::FindPoseParam(const FString& Name) const
{
	for (int32 i = 0; i < PoseParams.Num(); ++i)
	{
		if (PoseParams[i].Name.Equals(Name, ESearchCase::IgnoreCase))
		{
			return i;
		}
	}
	return INDEX_NONE;
}

int32 FSourceMDLFile::FindOrAddPoseParam(const FSourcePoseParam& Param)
{
	const int32 Found = FindPoseParam(Param.Name);
	if (Found != INDEX_NONE)
	{
		return Found;
	}
	PoseParams.Add(Param);
	return PoseParams.Num() - 1;
}

void FSourceMDLFile::ReadBones(const TArray<uint8>& Mdl)
{
	const int32 NumBonesInFile = ReadInt(Mdl, MDL::OFF_NUMBONES);
	const int32 BoneIndex = ReadInt(Mdl, MDL::OFF_BONEINDEX);
	Bones.Reserve(NumBonesInFile);

	for (int32 i = 0; i < NumBonesInFile; ++i)
	{
		const int64 Off = BoneIndex + (int64)i * MDL::SIZE_BONE;
		FSourceStudioBone& Bone = Bones.AddDefaulted_GetRef();
		Bone.Name = ReadCString(Mdl, Off + ReadInt(Mdl, Off));
		Bone.Parent = ReadInt(Mdl, Off + MDL::BONE_OFF_PARENT);
		Bone.Pos = ReadVec3(Mdl, Off + MDL::BONE_OFF_POS);
		Bone.Quat = ReadQuat(Mdl, Off + MDL::BONE_OFF_QUAT);
		Bone.Rot = ReadVec3(Mdl, Off + MDL::BONE_OFF_ROT);
		Bone.PosScale = ReadVec3(Mdl, Off + MDL::BONE_OFF_POSSCALE);
		Bone.RotScale = ReadVec3(Mdl, Off + MDL::BONE_OFF_ROTSCALE);
		Bone.PoseToBone = ReadMatrix3x4(Mdl, Off + MDL::BONE_OFF_POSETOBONE);
		Bone.Alignment = ReadQuat(Mdl, Off + MDL::BONE_OFF_ALIGNMENT);
		Bone.Flags = ReadInt(Mdl, Off + MDL::BONE_OFF_FLAGS);
	}
}

void FSourceMDLFile::ReadSequences(const TArray<uint8>& Mdl)
{
	const int32 NumAnims = ReadInt(Mdl, MDL::OFF_NUMLOCALANIM);
	const int32 AnimIndex = ReadInt(Mdl, MDL::OFF_LOCALANIMINDEX);
	AnimDescs.Reserve(NumAnims);
	for (int32 i = 0; i < NumAnims; ++i)
	{
		const int64 Off = AnimIndex + (int64)i * MDL::SIZE_ANIMDESC;
		FSourceStudioAnimDesc& Anim = AnimDescs.AddDefaulted_GetRef();
		Anim.FileOffset = Off;
		Anim.Name = ReadCString(Mdl, Off + ReadInt(Mdl, Off + MDL::ANIM_OFF_NAMEINDEX));
		Anim.Fps = ReadFloat(Mdl, Off + MDL::ANIM_OFF_FPS);
		Anim.Flags = ReadInt(Mdl, Off + MDL::ANIM_OFF_FLAGS);
		Anim.NumFrames = ReadInt(Mdl, Off + MDL::ANIM_OFF_NUMFRAMES);
		Anim.AnimBlock = ReadInt(Mdl, Off + MDL::ANIM_OFF_ANIMBLOCK);
		Anim.AnimIndex = ReadInt(Mdl, Off + MDL::ANIM_OFF_ANIMINDEX);
		Anim.SectionIndex = ReadInt(Mdl, Off + MDL::ANIM_OFF_SECTIONINDEX);
		Anim.SectionFrames = ReadInt(Mdl, Off + MDL::ANIM_OFF_SECTIONFRAMES);

		// Long animations are cut into sections of SectionFrames frames, each addressed by its own (block, index);
		// mstudioanimdesc_t::pAnim allocates numframes / sectionframes + 2 of them (the last frame is stored alone).
		if (Anim.SectionFrames > 0 && Anim.SectionIndex != 0)
		{
			const int32 NumSections = Anim.NumFrames / Anim.SectionFrames + 2;
			for (int32 sct = 0; sct < NumSections; ++sct)
			{
				const int64 SOff = Off + Anim.SectionIndex + (int64)sct * MDL::SIZE_ANIMSECTION;
				Anim.Sections.Emplace(ReadInt(Mdl, SOff), ReadInt(Mdl, SOff + 4));
			}
		}

		const int32 NumMovements = ReadInt(Mdl, Off + MDL::ANIM_OFF_NUMMOVEMENTS);
		const int32 MovementIndex = ReadInt(Mdl, Off + MDL::ANIM_OFF_MOVEMENTINDEX);
		for (int32 mv = 0; mv < NumMovements && MovementIndex != 0; ++mv)
		{
			const int64 MOff = Off + MovementIndex + (int64)mv * MDL::SIZE_MOVEMENT;
			FSourceStudioMovement& Movement = Anim.Movements.AddDefaulted_GetRef();
			Movement.EndFrame = ReadInt(Mdl, MOff);
			Movement.MotionFlags = ReadInt(Mdl, MOff + 4);
			Movement.Position = ReadVec3(Mdl, MOff + MDL::MOVEMENT_OFF_POSITION);
		}
	}

	const int32 NumSeqs = ReadInt(Mdl, MDL::OFF_NUMLOCALSEQ);
	const int32 SeqIndex = ReadInt(Mdl, MDL::OFF_LOCALSEQINDEX);
	Sequences.Reserve(NumSeqs);
	for (int32 i = 0; i < NumSeqs; ++i)
	{
		const int64 Off = SeqIndex + (int64)i * MDL::SIZE_SEQDESC;
		FSourceStudioSequence& Seq = Sequences.AddDefaulted_GetRef();
		Seq.Label = ReadCString(Mdl, Off + ReadInt(Mdl, Off + MDL::SEQ_OFF_LABELINDEX));
		Seq.ActivityName = ReadCString(Mdl, Off + ReadInt(Mdl, Off + MDL::SEQ_OFF_ACTIVITYNAMEINDEX));
		Seq.Flags = ReadInt(Mdl, Off + MDL::SEQ_OFF_FLAGS);
		Seq.ActivityWeight = ReadInt(Mdl, Off + MDL::SEQ_OFF_ACTWEIGHT);
		Seq.NumBlends = ReadInt(Mdl, Off + MDL::SEQ_OFF_NUMBLENDS);
		Seq.FadeInTime = ReadFloat(Mdl, Off + MDL::SEQ_OFF_FADEINTIME);
		Seq.FadeOutTime = ReadFloat(Mdl, Off + MDL::SEQ_OFF_FADEOUTTIME);

		// animindexindex points at a short[] of animdesc indices, a groupsize[0] x groupsize[1] grid selected
		// by the two pose parameters. The whole grid is kept; AnimDescIndex holds its centre as the value for
		// anyone who drives nothing - an aim matrix runs aim-hard-one-way to aim-hard-the-other with
		// straight-ahead at its centre, and a model sampled at a corner stands with its arms wrenched over its
		// head, which is exactly how the first crouch came out.
		const int32 AnimIndexIndex = ReadInt(Mdl, Off + MDL::SEQ_OFF_ANIMINDEXINDEX);
		if (AnimIndexIndex != 0)
		{
			Seq.GroupSize[0] = FMath::Clamp(ReadInt(Mdl, Off + MDL::SEQ_OFF_GROUPSIZE0), 1, 64);
			Seq.GroupSize[1] = FMath::Clamp(ReadInt(Mdl, Off + MDL::SEQ_OFF_GROUPSIZE1), 1, 64);
			Seq.ParamIndex[0] = ReadInt(Mdl, Off + MDL::SEQ_OFF_PARAMINDEX0);
			Seq.ParamIndex[1] = ReadInt(Mdl, Off + MDL::SEQ_OFF_PARAMINDEX1);
			Seq.ParamStart[0] = ReadFloat(Mdl, Off + MDL::SEQ_OFF_PARAMSTART0);
			Seq.ParamStart[1] = ReadFloat(Mdl, Off + MDL::SEQ_OFF_PARAMSTART1);
			Seq.ParamEnd[0] = ReadFloat(Mdl, Off + MDL::SEQ_OFF_PARAMEND0);
			Seq.ParamEnd[1] = ReadFloat(Mdl, Off + MDL::SEQ_OFF_PARAMEND1);
			for (int32 Cell = 0; Cell < Seq.GroupSize[0] * Seq.GroupSize[1]; ++Cell)
			{
				const int32 Resolved = ReadI16(Mdl, Off + AnimIndexIndex + (int64)Cell * 2);
				Seq.BlendAnimDescs.Add(AnimDescs.IsValidIndex(Resolved) ? Resolved : INDEX_NONE);
			}
			const int32 Middle = (Seq.GroupSize[1] / 2) * Seq.GroupSize[0] + (Seq.GroupSize[0] / 2);
			Seq.AnimDescIndex = Seq.BlendAnimDescs.IsValidIndex(Middle) ? Seq.BlendAnimDescs[Middle] : INDEX_NONE;
		}

		// Autolayers (gestures are built from these) and the per-bone weight list.
		const int32 NumAutoLayers = ReadInt(Mdl, Off + MDL::SEQ_OFF_NUMAUTOLAYERS);
		const int32 AutoLayerIndex = ReadInt(Mdl, Off + MDL::SEQ_OFF_AUTOLAYERINDEX);
		for (int32 al = 0; al < NumAutoLayers && AutoLayerIndex != 0; ++al)
		{
			const int64 ALOff = Off + AutoLayerIndex + (int64)al * MDL::SIZE_AUTOLAYER;
			FSourceStudioAutoLayer& Layer = Seq.AutoLayers.AddDefaulted_GetRef();
			Layer.Sequence = ReadI16(Mdl, ALOff);
			Layer.Pose = ReadI16(Mdl, ALOff + 2);
			Layer.Flags = ReadInt(Mdl, ALOff + 4);
			Layer.Start = ReadFloat(Mdl, ALOff + 8);
			Layer.Peak = ReadFloat(Mdl, ALOff + 12);
			Layer.Tail = ReadFloat(Mdl, ALOff + 16);
			Layer.End = ReadFloat(Mdl, ALOff + 20);
		}
		const int32 WeightListIndex = ReadInt(Mdl, Off + MDL::SEQ_OFF_WEIGHTLISTINDEX);
		if (WeightListIndex != 0)
		{
			for (int32 bn = 0; bn < Bones.Num(); ++bn)
			{
				Seq.BoneWeights.Add(ReadFloat(Mdl, Off + WeightListIndex + (int64)bn * 4));
			}
		}

		const int32 NumEvents = ReadInt(Mdl, Off + MDL::SEQ_OFF_NUMEVENTS);
		const int32 EventIndex = ReadInt(Mdl, Off + MDL::SEQ_OFF_EVENTINDEX);
		for (int32 e = 0; e < NumEvents && EventIndex != 0; ++e)
		{
			const int64 EvOff = Off + EventIndex + (int64)e * MDL::SIZE_EVENT;
			FSourceStudioEvent& Event = Seq.Events.AddDefaulted_GetRef();
			Event.Cycle = ReadFloat(Mdl, EvOff);
			Event.Event = ReadInt(Mdl, EvOff + MDL::EVENT_OFF_EVENT);
			Event.Options = ReadCString(Mdl, EvOff + MDL::EVENT_OFF_OPTIONS);
			const int32 NameIndex = ReadInt(Mdl, EvOff + MDL::EVENT_OFF_NAMEINDEX);
			if (NameIndex != 0)
			{
				Event.Name = ReadCString(Mdl, EvOff + NameIndex);
			}
		}
	}
}

void FSourceMDLFile::ReadAttachments(const TArray<uint8>& Mdl)
{
	const int32 Num = ReadInt(Mdl, MDL::OFF_NUMLOCALATTACHMENTS);
	const int32 Index = ReadInt(Mdl, MDL::OFF_LOCALATTACHMENTINDEX);
	Attachments.Reserve(Num);
	for (int32 i = 0; i < Num; ++i)
	{
		const int64 Off = Index + (int64)i * MDL::SIZE_ATTACHMENT;
		FSourceStudioAttachment& Attach = Attachments.AddDefaulted_GetRef();
		Attach.Name = ReadCString(Mdl, Off + ReadInt(Mdl, Off));
		Attach.Bone = ReadInt(Mdl, Off + MDL::ATTACH_OFF_BONE);
		Attach.Local = ReadMatrix3x4(Mdl, Off + MDL::ATTACH_OFF_LOCAL);
	}
}

// ---- Sequence selection ----

int32 FSourceMDLFile::FindSequenceByLabel(const FString& Label) const
{
	for (int32 i = 0; i < Sequences.Num(); ++i)
	{
		if (Sequences[i].Label.Equals(Label, ESearchCase::IgnoreCase))
		{
			return i;
		}
	}
	return INDEX_NONE;
}

int32 FSourceMDLFile::SelectWeightedSequence(const FString& ActivityName) const
{
	// CStudioHdr::SelectWeightedSequence: reservoir-sample the matching sequences by actweight, so a weapon with
	// several recoil animations does not play the same one every shot.
	int32 Chosen = INDEX_NONE;
	int32 TotalWeight = 0;
	for (int32 i = 0; i < Sequences.Num(); ++i)
	{
		const FSourceStudioSequence& Seq = Sequences[i];
		if (Seq.ActivityName.IsEmpty() || !Seq.ActivityName.Equals(ActivityName, ESearchCase::IgnoreCase))
		{
			continue;
		}
		const int32 Weight = FMath::Max(1, Seq.ActivityWeight);
		TotalWeight += Weight;
		if (Chosen == INDEX_NONE || FMath::RandRange(0, TotalWeight - 1) < Weight)
		{
			Chosen = i;
		}
	}
	return Chosen;
}

float FSourceMDLFile::GetSequenceDuration(int32 SequenceIndex) const
{
	if (!Sequences.IsValidIndex(SequenceIndex))
	{
		return 0.0f;
	}
	const int32 AnimIdx = Sequences[SequenceIndex].AnimDescIndex;
	if (!AnimDescs.IsValidIndex(AnimIdx))
	{
		return 0.0f;
	}
	// Studio_Duration: the last frame closes the loop back onto the first, so it is not counted.
	const FSourceStudioAnimDesc& Anim = AnimDescs[AnimIdx];
	return Anim.Fps > 0.0f ? (Anim.NumFrames - 1) / Anim.Fps : 0.0f;
}

bool FSourceMDLFile::IsSequenceLooping(int32 SequenceIndex) const
{
	return Sequences.IsValidIndex(SequenceIndex) && (Sequences[SequenceIndex].Flags & STUDIO_LOOPING) != 0;
}

float FSourceMDLFile::GetSequenceGroundSpeed(int32 SequenceIndex) const
{
	// Studio_SeqMovement sums the animation's movement blocks; the motor divides that distance by the sequence's
	// duration. This is how a Source NPC's run speed is authored - in the animation, not in code.
	if (!Sequences.IsValidIndex(SequenceIndex) || !AnimDescs.IsValidIndex(Sequences[SequenceIndex].AnimDescIndex))
	{
		return 0.0f;
	}
	const FSourceStudioAnimDesc& Anim = AnimDescs[Sequences[SequenceIndex].AnimDescIndex];
	FVector3f Total = FVector3f::ZeroVector;
	for (const FSourceStudioMovement& Movement : Anim.Movements)
	{
		Total += Movement.Position;
	}
	const float Duration = GetSequenceDuration(SequenceIndex);
	return Duration > 0.0f ? Total.Size() / Duration : 0.0f;
}

// ---- Pose evaluation ----

void FSourceMDLFile::EvaluateBindPose(TArray<FSourceMatrix3x4>& OutBoneToModel) const
{
	OutBoneToModel.SetNum(Bones.Num());
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		const FSourceMatrix3x4 Local = FSourceMatrix3x4::FromQuatPos(Bones[i].Quat, Bones[i].Pos);
		OutBoneToModel[i] = (Bones[i].Parent >= 0 && Bones[i].Parent < i)
			? OutBoneToModel[Bones[i].Parent].Concat(Local)
			: Local;
	}
}

namespace
{
	// mathlib quaternion helpers, in Source's conventions (bone_setup.cpp uses these for layer blending).

	/** QuaternionAlign: flips q if it is on the far side of p, so blends take the short way round. */
	FQuat4f QuaternionAlign(const FQuat4f& P, const FQuat4f& Q)
	{
		return ((P | Q) < 0.0f) ? FQuat4f(-Q.X, -Q.Y, -Q.Z, -Q.W) : Q;
	}

	/** QuaternionMult(p, q): p * q, q aligned to p first. */
	FQuat4f QuaternionMult(const FQuat4f& P, const FQuat4f& Q)
	{
		return P * QuaternionAlign(P, Q);
	}

	/** QuaternionScale(p, t): the rotation p scaled to a fraction t of its angle. */
	FQuat4f QuaternionScale(const FQuat4f& P, float T)
	{
		float Sinom = FMath::Sqrt(P.X * P.X + P.Y * P.Y + P.Z * P.Z);
		Sinom = FMath::Min(Sinom, 1.0f);
		const float Sinsom = FMath::Sin(FMath::Asin(Sinom) * T);
		const float Scale = Sinsom / (Sinom + FLT_EPSILON);
		float R = 1.0f - Sinsom * Sinsom;
		R = FMath::Sqrt(FMath::Max(R, 0.0f));
		return FQuat4f(P.X * Scale, P.Y * Scale, P.Z * Scale, P.W < 0.0f ? -R : R);
	}

	/** QuaternionMA(p, s, q): p * (q scaled by s). */
	FQuat4f QuaternionMA(const FQuat4f& P, float S, const FQuat4f& Q)
	{
		return QuaternionMult(P, QuaternionScale(Q, S)).GetNormalized();
	}

	/** QuaternionSM(s, p, q): (p scaled by s) * q. */
	FQuat4f QuaternionSM(float S, const FQuat4f& P, const FQuat4f& Q)
	{
		return QuaternionMult(QuaternionScale(P, S), Q).GetNormalized();
	}

	float SimpleSpline(float Value)
	{
		const float ValueSquared = Value * Value;
		return 3.0f * ValueSquared - 2.0f * ValueSquared * Value;
	}
}

void FSourceMDLFile::InitLocalPose(FSourceLocalPose& Pose) const
{
	Pose.Quat.SetNum(Bones.Num());
	Pose.Pos.SetNum(Bones.Num());
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		Pose.Quat[i] = Bones[i].Quat;
		Pose.Pos[i] = Bones[i].Pos;
	}
}

void FSourceMDLFile::BuildBoneToModel(const FSourceLocalPose& Pose, TArray<FSourceMatrix3x4>& OutBoneToModel) const
{
	// studiomdl orders the table so a parent always precedes its children.
	OutBoneToModel.SetNum(Bones.Num());
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		const FSourceMatrix3x4 Local = FSourceMatrix3x4::FromQuatPos(Pose.Quat[i], Pose.Pos[i]);
		OutBoneToModel[i] = (Bones[i].Parent >= 0 && Bones[i].Parent < i)
			? OutBoneToModel[Bones[i].Parent].Concat(Local)
			: Local;
	}
}

void FSourceMDLFile::LocalPoseParameter(const FSourceStudioSequence& Seq, int32 Axis,
	const TArray<float>* PoseParamValues, int32& OutIndex, float& OutFraction) const
{
	// Studio_LocalPoseParameter. The cell the value sits in, and how far through that cell it is - which is
	// what turns a grid lookup into a blend. Source normalises through the pose parameter's own range and then
	// through the sequence's sub-range; the two divisions cancel to this, which also copes with the reversed
	// ranges some sequences are authored with.
	OutIndex = 0;
	OutFraction = 0.0f;
	const int32 Size = Seq.GroupSize[Axis];
	const int32 Param = Seq.ParamIndex[Axis];
	if (Size <= 1)
	{
		return;
	}
	float Setting = 0.5f;	// undriven: the middle of the grid, where straight-ahead lives
	if (PoseParamValues && Param >= 0 && PoseParamValues->IsValidIndex(Param))
	{
		const float Span = Seq.ParamEnd[Axis] - Seq.ParamStart[Axis];
		if (!FMath::IsNearlyZero(Span))
		{
			Setting = FMath::Clamp(((*PoseParamValues)[Param] - Seq.ParamStart[Axis]) / Span, 0.0f, 1.0f);
		}
	}
	// The last cell has no cell above it to blend toward, so it is approached as the far end of the one below.
	OutIndex = FMath::Clamp((int32)(Setting * (Size - 1)), 0, Size - 2);
	OutFraction = FMath::Clamp(Setting * (Size - 1) - OutIndex, 0.0f, 1.0f);
}

namespace
{
	/** BlendBones: q1/pos1 moved toward q2/pos2 by S, in place. */
	void BlendPose(FSourceLocalPose& A, const FSourceLocalPose& B, float S)
	{
		if (S <= 0.0f)
		{
			return;
		}
		if (S >= 1.0f)
		{
			A = B;
			return;
		}
		const int32 Num = FMath::Min(A.Quat.Num(), B.Quat.Num());
		for (int32 i = 0; i < Num; ++i)
		{
			A.Quat[i] = FQuat4f::Slerp(A.Quat[i], ((A.Quat[i] | B.Quat[i]) < 0.0f)
				? FQuat4f(-B.Quat[i].X, -B.Quat[i].Y, -B.Quat[i].Z, -B.Quat[i].W) : B.Quat[i], S);
			A.Quat[i].Normalize();
			A.Pos[i] = FMath::Lerp(A.Pos[i], B.Pos[i], S);
		}
	}
}

bool FSourceMDLFile::CalcPoseSingle(int32 SequenceIndex, float Cycle, FSourceLocalPose& Out,
	const TArray<float>* PoseParamValues) const
{
	if (!Sequences.IsValidIndex(SequenceIndex) || Bones.Num() == 0)
	{
		return false;
	}
	const FSourceStudioSequence& Seq = Sequences[SequenceIndex];

	// A single-cell sequence is just its animation. Anything larger is a blend grid steered by up to two pose
	// parameters, and the pose is interpolated across the cells the value falls between rather than snapped to
	// the nearest - four of them at a corner-to-corner blend, which is what an aim matrix is.
	if (Seq.BlendAnimDescs.Num() <= 1)
	{
		return CalcAnimation(Seq.AnimDescIndex, Cycle, Out);
	}

	int32 I0 = 0, I1 = 0;
	float S0 = 0.0f, S1 = 0.0f;
	LocalPoseParameter(Seq, 0, PoseParamValues, I0, S0);
	LocalPoseParameter(Seq, 1, PoseParamValues, I1, S1);

	auto CellAnim = [&Seq](int32 X, int32 Y)
	{
		X = FMath::Clamp(X, 0, Seq.GroupSize[0] - 1);
		Y = FMath::Clamp(Y, 0, Seq.GroupSize[1] - 1);
		const int32 Index = Y * Seq.GroupSize[0] + X;
		return Seq.BlendAnimDescs.IsValidIndex(Index) ? Seq.BlendAnimDescs[Index] : INDEX_NONE;
	};

	// The row the value sits in, blended along the first axis...
	if (!CalcAnimation(CellAnim(I0, I1), Cycle, Out))
	{
		return false;
	}
	if (S0 > 0.001f)
	{
		FSourceLocalPose Next;
		if (CalcAnimation(CellAnim(I0 + 1, I1), Cycle, Next))
		{
			BlendPose(Out, Next, S0);
		}
	}
	// ...then the row above it, blended the same way, and the two rows blended together.
	if (S1 > 0.001f && Seq.GroupSize[1] > 1)
	{
		FSourceLocalPose Upper;
		if (CalcAnimation(CellAnim(I0, I1 + 1), Cycle, Upper))
		{
			if (S0 > 0.001f)
			{
				FSourceLocalPose UpperNext;
				if (CalcAnimation(CellAnim(I0 + 1, I1 + 1), Cycle, UpperNext))
				{
					BlendPose(Upper, UpperNext, S0);
				}
			}
			BlendPose(Out, Upper, S1);
		}
	}
	return true;
}

bool FSourceMDLFile::CalcAnimation(int32 AnimDescIndex, float Cycle, FSourceLocalPose& Out) const
{
	if (!AnimDescs.IsValidIndex(AnimDescIndex) || Bones.Num() == 0)
	{
		return false;
	}
	const FSourceStudioAnimDesc& Anim = AnimDescs[AnimDescIndex];

	// Bones the animation never moves keep the bind pose - or, for a delta animation, no change
	// at all (identity rotation, zero offset), since the whole thing is added to whatever is underneath.
	const bool bDelta = (Anim.Flags & STUDIO::SEQ_DELTA) != 0;
	Out.Quat.SetNum(Bones.Num());
	Out.Pos.SetNum(Bones.Num());
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		Out.Quat[i] = bDelta ? FQuat4f::Identity : Bones[i].Quat;
		Out.Pos[i] = bDelta ? FVector3f::ZeroVector : Bones[i].Pos;
	}

	// Studio_CalcFrame: the cycle spans [0, numframes - 1]. We sample the nearest frame rather than blending
	// between two, which is what Source does when its interpolation fraction is below 0.001.
	const int32 MaxFrame = FMath::Max(0, Anim.NumFrames - 1);
	int32 Frame = FMath::Clamp(FMath::RoundToInt(FMath::Clamp(Cycle, 0.0f, 1.0f) * MaxFrame), 0, MaxFrame);

	// mstudioanimdesc_t::pAnim: resolve which block holds this frame's data, and the frame within it.
	int32 Block = Anim.AnimBlock;
	int32 Index = Anim.AnimIndex;
	if (Anim.SectionFrames > 0 && Anim.Sections.Num() > 0)
	{
		int32 Section;
		if (Anim.NumFrames > Anim.SectionFrames && Frame == Anim.NumFrames - 1)
		{
			// the last frame of a long animation is stored separately
			Frame = 0;
			Section = Anim.NumFrames / Anim.SectionFrames + 1;
		}
		else
		{
			Section = Frame / Anim.SectionFrames;
			Frame -= Section * Anim.SectionFrames;
		}
		if (!Anim.Sections.IsValidIndex(Section))
		{
			return true;
		}
		Block = Anim.Sections[Section].Key;
		Index = Anim.Sections[Section].Value;
	}

	// A borrowed sequence's frames live in the library that authored it, and its bone indices are the
	// library's; the group's map says which of our bones each one is.
	const FSourceMDLFile* DataModel = this;
	const TArray<int32>* BoneMap = nullptr;
	if (Anim.Group > 0 && IncludeGroups.IsValidIndex(Anim.Group - 1))
	{
		DataModel = IncludeGroups[Anim.Group - 1].File.Get();
		BoneMap = &IncludeGroups[Anim.Group - 1].BoneMap;
	}

	// Block 0 is inside the .mdl, relative to the animdesc; any other block is a byte range of the .ani file.
	const TArray<uint8>* Src = nullptr;
	int64 P = 0;
	if (Block == 0)
	{
		Src = &DataModel->MdlData;
		P = Anim.FileOffset + Index;
	}
	else if (DataModel->AnimBlocks.IsValidIndex(Block) && DataModel->AniData.Num() > 0)
	{
		Src = &DataModel->AniData;
		P = (int64)DataModel->AnimBlocks[Block].Key + Index;
	}
	if (!Src || Block < 0 || P <= 0 || P >= Src->Num())
	{
		return true;	// STUDIO_ALLZEROS and friends: no data, the init pose stands
	}

	for (int32 Guard = 0; Guard <= DataModel->Bones.Num(); ++Guard)
	{
		uint8 BoneIdx = 0, Flags = 0;
		if (!Peek((*Src), P, BoneIdx) || !Peek((*Src), P + 1, Flags) || BoneIdx >= DataModel->Bones.Num())
		{
			break;
		}
		const FSourceStudioBone& Bone = DataModel->Bones[BoneIdx];
		const int16 NextOffset = ReadI16((*Src), P + 2);
		const int64 Data = P + STUDIOANIM::SIZE_RLE_HEADER;

		// CalcBoneQuaternion
		FQuat4f Q;
		if (Flags & STUDIOANIM::FLAG_RAWROT)
		{
			Q = ReadQuat48((*Src), Data);
		}
		else if (Flags & STUDIOANIM::FLAG_RAWROT2)
		{
			Q = ReadQuat64((*Src), Data);
		}
		else if (Flags & STUDIOANIM::FLAG_ANIMROT)
		{
			FVector3f Angle(0.0f);
			for (int32 j = 0; j < 3; ++j)
			{
				const int16 RunOffset = ReadI16((*Src), Data + 2 * j);
				Angle[j] = ExtractAnimValue((*Src), RunOffset > 0 ? Data + RunOffset : 0, Frame, Bone.RotScale[j]);
			}
			if (!(Flags & STUDIOANIM::FLAG_DELTA))
			{
				Angle += Bone.Rot;
			}
			Q = AngleQuaternion(Angle);
		}
		else
		{
			Q = (Flags & STUDIOANIM::FLAG_DELTA) ? FQuat4f::Identity : Bone.Quat;
		}

		// CalcBonePosition
		FVector3f Pos(0.0f);
		if (Flags & STUDIOANIM::FLAG_RAWPOS)
		{
			const int64 PosOff = Data
				+ ((Flags & STUDIOANIM::FLAG_RAWROT) ? STUDIOANIM::SIZE_QUAT48 : 0)
				+ ((Flags & STUDIOANIM::FLAG_RAWROT2) ? STUDIOANIM::SIZE_QUAT64 : 0);
			Pos = ReadVector48((*Src), PosOff);
		}
		else if (Flags & STUDIOANIM::FLAG_ANIMPOS)
		{
			// The position value pointers follow the rotation ones when both are animated.
			const int64 PosV = Data + ((Flags & STUDIOANIM::FLAG_ANIMROT) ? STUDIOANIM::SIZE_VALUEPTR : 0);
			for (int32 j = 0; j < 3; ++j)
			{
				const int16 RunOffset = ReadI16((*Src), PosV + 2 * j);
				Pos[j] = ExtractAnimValue((*Src), RunOffset > 0 ? PosV + RunOffset : 0, Frame, Bone.PosScale[j]);
			}
			if (!(Flags & STUDIOANIM::FLAG_DELTA))
			{
				Pos += Bone.Pos;
			}
		}
		else
		{
			Pos = (Flags & STUDIOANIM::FLAG_DELTA) ? FVector3f::ZeroVector : Bone.Pos;
		}

		const int32 OurBone = BoneMap ? (*BoneMap)[BoneIdx] : BoneIdx;
		if (OurBone != INDEX_NONE)
		{
			Out.Quat[OurBone] = Q;
			Out.Pos[OurBone] = Pos;
		}

		if (NextOffset == 0)
		{
			break;
		}
		P += NextOffset;
	}

	return true;
}

bool FSourceMDLFile::IsSequenceDelta(int32 SequenceIndex) const
{
	if (!Sequences.IsValidIndex(SequenceIndex))
	{
		return false;
	}
	const FSourceStudioSequence& Seq = Sequences[SequenceIndex];
	if (Seq.Flags & STUDIO::SEQ_DELTA)
	{
		return true;
	}
	return AnimDescs.IsValidIndex(Seq.AnimDescIndex) && (AnimDescs[Seq.AnimDescIndex].Flags & STUDIO::SEQ_DELTA) != 0;
}

float FSourceMDLFile::SequenceBoneWeight(const FSourceStudioSequence& Seq, int32 Bone) const
{
	if (Seq.BoneWeights.Num() == 0)
	{
		return 1.0f;	// no $weightlist: the sequence moves everything
	}
	if (Seq.WeightGroup == 0)
	{
		return Seq.BoneWeights.IsValidIndex(Bone) ? Seq.BoneWeights[Bone] : 1.0f;
	}
	if (!IncludeGroups.IsValidIndex(Seq.WeightGroup - 1))
	{
		return 1.0f;
	}
	const TArray<int32>& HostToInclude = IncludeGroups[Seq.WeightGroup - 1].HostToInclude;
	const int32 Theirs = HostToInclude.IsValidIndex(Bone) ? HostToInclude[Bone] : INDEX_NONE;
	return (Theirs != INDEX_NONE && Seq.BoneWeights.IsValidIndex(Theirs)) ? Seq.BoneWeights[Theirs] : 1.0f;
}

void FSourceMDLFile::SlerpBones(FSourceLocalPose& Pose, const FSourceStudioSequence& Seq, const FSourceLocalPose& Layer, float Weight, bool bDelta) const
{
	const bool bPost = (Seq.Flags & STUDIO::SEQ_POST) != 0;
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		float S = Weight * SequenceBoneWeight(Seq, i);
		if (S <= 0.0f)
		{
			continue;
		}
		S = FMath::Min(S, 1.0f);
		if (bDelta)
		{
			// delta sequence: q1 = q1 * q2^s (POST) or q2^s * q1; pos1 += pos2 * s
			Pose.Quat[i] = bPost ? QuaternionMA(Pose.Quat[i], S, Layer.Quat[i]) : QuaternionSM(S, Layer.Quat[i], Pose.Quat[i]);
			Pose.Pos[i] += Layer.Pos[i] * S;
		}
		else if (S >= 1.0f)
		{
			Pose.Quat[i] = Layer.Quat[i];
			Pose.Pos[i] = Layer.Pos[i];
		}
		else
		{
			Pose.Quat[i] = FQuat4f::Slerp(Pose.Quat[i], QuaternionAlign(Pose.Quat[i], Layer.Quat[i]), S);
			Pose.Pos[i] = FMath::Lerp(Pose.Pos[i], Layer.Pos[i], S);
		}
	}
}

void FSourceMDLFile::AddSequenceLayers(FSourceLocalPose& Pose, int32 SequenceIndex, float Cycle, float Weight,
	const TArray<float>* PoseParamValues) const
{
	const FSourceStudioSequence& Seq = Sequences[SequenceIndex];
	for (const FSourceStudioAutoLayer& Layer : Seq.AutoLayers)
	{
		if (Layer.Flags & STUDIO::AL_LOCAL)
		{
			continue;
		}
		// The ramp's driver: the parent's cycle for an ordinary layer, and for an AL_POSE layer the pose
		// parameter's value in the author's units - the aim matrices ride these, weighted in by how far the
		// aim is from centre (bone_setup.cpp's AddSequenceLayers). A pose-driven layer with no driven value
		// keeps the old behaviour and stays out.
		float RampIndex = Cycle;
		if (Layer.Flags & STUDIO::AL_POSE)
		{
			if (!PoseParamValues || !PoseParamValues->IsValidIndex(Layer.Pose))
			{
				continue;
			}
			RampIndex = (*PoseParamValues)[Layer.Pose];
		}
		float LayerCycle = Cycle;
		float LayerWeight = Weight;
		if (Layer.Start != Layer.End)
		{
			float S = 1.0f;
			const float Index = RampIndex;
			if (Index < Layer.Start || Index >= Layer.End)
			{
				continue;
			}
			if (Index < Layer.Peak && Layer.Start != Layer.Peak)
			{
				S = (Index - Layer.Start) / (Layer.Peak - Layer.Start);
			}
			else if (Index > Layer.Tail && Layer.End != Layer.Tail)
			{
				S = (Layer.End - Index) / (Layer.End - Layer.Tail);
			}
			if (Layer.Flags & STUDIO::AL_SPLINE)
			{
				S = SimpleSpline(S);
			}
			if ((Layer.Flags & STUDIO::AL_XFADE) && Index > Layer.Tail)
			{
				LayerWeight = (S * Weight) / ((1.0f - Weight) + S * Weight);
			}
			else
			{
				LayerWeight = Weight * S;
			}
			LayerCycle = (Layer.Flags & STUDIO::AL_POSE) ? Cycle : (Cycle - Layer.Start) / (Layer.End - Layer.Start);
		}
		if (Layer.Flags & STUDIO::AL_NOBLEND)
		{
			LayerWeight = 1.0f;
		}
		AccumulateSequence(Pose, Layer.Sequence, LayerCycle, LayerWeight, PoseParamValues);
	}
}

bool FSourceMDLFile::AccumulateSequence(FSourceLocalPose& Pose, int32 SequenceIndex, float Cycle, float Weight,
	const TArray<float>* PoseParams_) const
{
	// AccumulatePose: the sequence's own pose, its layers on top of that, then blended into what is there.
	if (Weight <= 0.0f || Pose.Quat.Num() != Bones.Num())
	{
		return false;
	}
	FSourceLocalPose Single;
	if (!CalcPoseSingle(SequenceIndex, Cycle, Single, PoseParams_))
	{
		return false;
	}
	AddSequenceLayers(Single, SequenceIndex, Cycle, Weight, PoseParams_);
	SlerpBones(Pose, Sequences[SequenceIndex], Single, Weight, IsSequenceDelta(SequenceIndex));
	return true;
}

bool FSourceMDLFile::EvaluateSequence(int32 SequenceIndex, float Cycle, TArray<FSourceMatrix3x4>& OutBoneToModel) const
{
	FSourceLocalPose Pose;
	InitLocalPose(Pose);
	if (Bones.Num() == 0 || !AccumulateSequence(Pose, SequenceIndex, Cycle, 1.0f))
	{
		EvaluateBindPose(OutBoneToModel);
		return false;
	}
	BuildBoneToModel(Pose, OutBoneToModel);
	return true;
}

void FSourceMDLFile::ApplyPose(const TArray<FSourceMatrix3x4>& BoneToModel)
{
	if (SkinVertices.Num() != Sections.Num() || BoneToModel.Num() != Bones.Num() || Bones.Num() == 0)
	{
		return;
	}

	// CStudioRender builds one matrix per bone that takes a reference-pose vertex straight to posed model space.
	TArray<FSourceMatrix3x4> SkinMatrices;
	SkinMatrices.SetNum(Bones.Num());
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		SkinMatrices[i] = BoneToModel[i].Concat(Bones[i].PoseToBone);
	}

	for (int32 s = 0; s < Sections.Num(); ++s)
	{
		FSourceMeshSection& Section = Sections[s];
		const TArray<FSourceSkinVertex>& Skin = SkinVertices[s];
		const int32 Count = FMath::Min(Section.Vertices.Num(), Skin.Num());
		for (int32 v = 0; v < Count; ++v)
		{
			const FSourceSkinVertex& In = Skin[v];
			FVector3f Pos = FVector3f::ZeroVector;
			FVector3f Nrm = FVector3f::ZeroVector;
			for (int32 b = 0; b < In.NumBones; ++b)
			{
				const uint8 BoneIdx = In.Bones[b];
				if (BoneIdx >= SkinMatrices.Num())
				{
					continue;
				}
				const FSourceMatrix3x4& Mat = SkinMatrices[BoneIdx];
				Pos += Mat.TransformPosition(In.Position) * In.Weights[b];
				Nrm += Mat.TransformVector(In.Normal) * In.Weights[b];
			}
			Section.Vertices[v] = FSourceCoords::ToUE(Pos, UnitScale);
			Section.Normals[v] = FSourceCoords::ToUEDirection(Nrm.GetSafeNormal());
		}
	}
}

bool FSourceMDLFile::GetAttachment(const FString& Name, const TArray<FSourceMatrix3x4>& BoneToModel,
	FVector& OutPosition, FVector& OutForward) const
{
	for (const FSourceStudioAttachment& Attach : Attachments)
	{
		if (!Attach.Name.Equals(Name, ESearchCase::IgnoreCase) || !BoneToModel.IsValidIndex(Attach.Bone))
		{
			continue;
		}
		const FSourceMatrix3x4 World = BoneToModel[Attach.Bone].Concat(Attach.Local);
		OutPosition = FSourceCoords::ToUE(World.GetOrigin(), UnitScale);
		OutForward = FSourceCoords::ToUEDirection(World.GetForward().GetSafeNormal());
		return true;
	}
	return false;
}

void FSourceMDLFile::CollectEvents(int32 SequenceIndex, float PrevCycle, float NewCycle, bool bLooping,
	TArray<const FSourceStudioEvent*>& OutEvents) const
{
	if (!Sequences.IsValidIndex(SequenceIndex))
	{
		return;
	}
	for (const FSourceStudioEvent& Event : Sequences[SequenceIndex].Events)
	{
		// C_BaseAnimating::DoAnimationEvents fires an event as the cycle crosses it, allowing for the wrap that a
		// looping sequence makes each time it restarts.
		const bool bCrossed = (bLooping && NewCycle < PrevCycle)
			? (Event.Cycle > PrevCycle || Event.Cycle <= NewCycle)
			: (Event.Cycle > PrevCycle && Event.Cycle <= NewCycle);
		if (bCrossed)
		{
			OutEvents.Add(&Event);
		}
	}
}
