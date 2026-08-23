#include "SourcePHYFile.h"
#include "LambdaFileSystem.h"
#include "LambdaSourceModule.h"
#include "SourceCoordinates.h"
#include "SourceKeyValues.h"

namespace
{
	// phyheader_t
	constexpr int32 PHY_HEADER_SIZE = 16;
	// compactsurfaceheader_t: int size; int vphysicsID; short version; short modelType; int surfaceSize;
	//                         Vector dragAxisAreas; int axisMapSize;
	constexpr int32 COMPACT_SURFACE_HEADER_SIZE = 32;
	constexpr int32 VPHYSICS_ID = 0x59485056;	// 'VPHY'
	// IVP_Compact_Surface: Vector mass_center; Vector rotation_inertia; float upper_limit_radius;
	//                      uint max_deviation:8 byte_size:24; int offset_ledgetree_root; int dummy[3];
	constexpr int32 IVP_COMPACT_SURFACE_SIZE = 48;
	constexpr int32 IVP_LEDGE_SIZE = 16;
	constexpr int32 IVP_TRIANGLE_SIZE = 16;
	constexpr int32 IVP_POINT_SIZE = 16;	// IVP_U_Float_Hesse: x, y, z, hesse_val
	// IVP_Compact_Ledgetree_node: int offset_right_node; int offset_compact_ledge; Vector center; float radius;
	//                             uchar box_sizes[3]; uchar free_0;
	constexpr int32 IVP_LEDGETREE_NODE_SIZE = 28;
	constexpr float IVP_METERS_TO_UNITS = 1.0f / 0.0254f;

	template <typename T>
	bool ReadAt(const TArray<uint8>& Bytes, int64 Offset, T& Out)
	{
		if (Offset < 0 || Offset + (int64)sizeof(T) > Bytes.Num())
		{
			return false;
		}
		FMemory::Memcpy(&Out, Bytes.GetData() + Offset, sizeof(T));
		return true;
	}

	/**
	 * Walks the ledge tree and collects the address of every ledge hanging off it. A node with no right child is a
	 * terminal one and names a ledge; otherwise its left child follows it immediately and its right child is that
	 * many bytes further on. The ledges cannot simply be read one after another: a concave collision model shares
	 * one pool of points between all of its ledges, so a ledge's own size covers points that sit past the next
	 * ledge - stepping by it walks straight into the middle of the triangles.
	 */
	void CollectLedges(const TArray<uint8>& Bytes, int64 Node, int64 SolidEnd, TArray<int64>& OutLedges, int32 Depth = 0)
	{
		if (Depth > 64 || Node < 0 || Node + IVP_LEDGETREE_NODE_SIZE > SolidEnd)
		{
			return;
		}
		int32 RightOffset = 0, LedgeOffset = 0;
		if (!ReadAt(Bytes, Node, RightOffset) || !ReadAt(Bytes, Node + 4, LedgeOffset))
		{
			return;
		}
		if (RightOffset == 0)
		{
			const int64 Ledge = Node + LedgeOffset;
			if (Ledge >= 0 && Ledge + IVP_LEDGE_SIZE <= SolidEnd)
			{
				OutLedges.AddUnique(Ledge);
			}
			return;
		}
		CollectLedges(Bytes, Node + IVP_LEDGETREE_NODE_SIZE, SolidEnd, OutLedges, Depth + 1);
		CollectLedges(Bytes, Node + RightOffset, SolidEnd, OutLedges, Depth + 1);
	}

	/** IVP (metres, y up) -> Source units -> UE cm/axes. */
	FVector IVPToUE(float X, float Y, float Z, float Scale)
	{
		const FVector3f Source(X * IVP_METERS_TO_UNITS, Z * IVP_METERS_TO_UNITS, -Y * IVP_METERS_TO_UNITS);
		return FSourceCoords::ToUE(Source, Scale);
	}
}

bool FSourcePHYFile::Load(const FString& RelativeModelPath, float Scale, FString* OutError)
{
	Solids.Reset();
	Constraints.Reset();
	bLoaded = false;

	FString PhyPath = RelativeModelPath;
	if (PhyPath.EndsWith(TEXT(".mdl"), ESearchCase::IgnoreCase))
	{
		PhyPath = PhyPath.LeftChop(4);
	}
	PhyPath += TEXT(".phy");

	TArray<uint8> Bytes;
	if (!FLambdaFileSystem::Get().ReadFile(PhyPath, Bytes))
	{
		if (OutError) { *OutError = FString::Printf(TEXT("no collision model '%s'"), *PhyPath); }
		return false;
	}

	int32 TextOffset = 0;
	if (!ParseBinary(Bytes, Scale, TextOffset, OutError) || !ParseText(Bytes, TextOffset, Scale, OutError))
	{
		return false;
	}

	bLoaded = true;
	UE_LOG(LogLambdaSource, Log, TEXT("Collision model '%s': %d solids, %d ragdoll constraints, total mass %.2f kg"),
		*PhyPath, Solids.Num(), Constraints.Num(), GetTotalMass());
	for (int32 SolidIndex = 0; SolidIndex < Solids.Num(); ++SolidIndex)
	{
		const FSourcePHYSolid& Solid = Solids[SolidIndex];
		FBox Bounds(ForceInit);
		int32 NumVerts = 0;
		for (const TArray<FVector>& Hull : Solid.Hulls)
		{
			for (const FVector& Vertex : Hull)
			{
				Bounds += Vertex;
				++NumVerts;
			}
		}
		UE_LOG(LogLambdaSource, Verbose, TEXT("  solid %d '%s': %d hulls, %d verts, %s .. %s units, %.2f kg"),
			SolidIndex, *Solid.BoneName, Solid.Hulls.Num(), NumVerts,
			*(Bounds.Min / Scale).ToCompactString(), *(Bounds.Max / Scale).ToCompactString(), Solid.Mass);
	}
	return true;
}

bool FSourcePHYFile::ParseBinary(const TArray<uint8>& Bytes, float Scale, int32& OutTextOffset, FString* OutError)
{
	int32 HeaderSize = 0, Id = 0, SolidCount = 0, Checksum = 0;
	if (!ReadAt(Bytes, 0, HeaderSize) || !ReadAt(Bytes, 4, Id) || !ReadAt(Bytes, 8, SolidCount) || !ReadAt(Bytes, 12, Checksum)
		|| HeaderSize < PHY_HEADER_SIZE || SolidCount <= 0 || SolidCount > 128)
	{
		if (OutError) { *OutError = TEXT("bad .phy header"); }
		return false;
	}

	int64 Offset = HeaderSize;
	for (int32 s = 0; s < SolidCount; ++s)
	{
		int32 Size = 0, VPhysicsId = 0, SurfaceSize = 0;
		if (!ReadAt(Bytes, Offset, Size) || !ReadAt(Bytes, Offset + 4, VPhysicsId) || !ReadAt(Bytes, Offset + 12, SurfaceSize))
		{
			if (OutError) { *OutError = FString::Printf(TEXT("solid %d: truncated"), s); }
			return false;
		}
		FSourcePHYSolid& Solid = Solids.AddDefaulted_GetRef();
		Solid.Index = s;

		if (VPhysicsId == VPHYSICS_ID)
		{
			const int64 Surface = Offset + COMPACT_SURFACE_HEADER_SIZE;
			float MassCenter[3] = { 0, 0, 0 };
			int32 LedgeTreeRoot = 0;
			if (ReadAt(Bytes, Surface, MassCenter) && ReadAt(Bytes, Surface + 32, LedgeTreeRoot))
			{
				Solid.MassCenter = IVPToUE(MassCenter[0], MassCenter[1], MassCenter[2], Scale);

				// Every convex piece of this solid, found through the ledge tree: a crate that is hollow is built
				// from one ledge per wall, and all of them have to be there or things fall through it.
				const int64 SolidEnd = FMath::Min<int64>(Offset + 4 + Size, Bytes.Num());
				TArray<int64> LedgeAddresses;
				CollectLedges(Bytes, Surface + LedgeTreeRoot, SolidEnd, LedgeAddresses);
				for (const int64 Ledge : LedgeAddresses)
				{
					int32 PointOffset = 0;
					int16 NumTriangles = 0;
					ReadAt(Bytes, Ledge, PointOffset);
					ReadAt(Bytes, Ledge + 12, NumTriangles);
					if (NumTriangles <= 0)
					{
						continue;
					}

					int32 MaxPoint = -1;
					for (int32 t = 0; t < NumTriangles; ++t)
					{
						const int64 Tri = Ledge + IVP_LEDGE_SIZE + (int64)t * IVP_TRIANGLE_SIZE;
						for (int32 e = 0; e < 3; ++e)
						{
							uint32 Edge = 0;
							if (ReadAt(Bytes, Tri + 4 + e * 4, Edge))
							{
								MaxPoint = FMath::Max(MaxPoint, (int32)(Edge & 0xFFFF));
							}
						}
					}

					TArray<FVector> Points;
					for (int32 p = 0; p <= MaxPoint; ++p)
					{
						float XYZ[3] = { 0, 0, 0 };
						if (ReadAt(Bytes, Ledge + PointOffset + (int64)p * IVP_POINT_SIZE, XYZ))
						{
							Points.Add(IVPToUE(XYZ[0], XYZ[1], XYZ[2], Scale));
						}
					}
					if (Points.Num() >= 4)
					{
						Solid.Hulls.Add(MoveTemp(Points));
					}
				}
			}
		}
		Offset += 4 + Size;
	}
	OutTextOffset = (int32)FMath::Min<int64>(Offset, Bytes.Num());
	return true;
}

bool FSourcePHYFile::ParseText(const TArray<uint8>& Bytes, int32 TextOffset, float Scale, FString* OutError)
{
	// The trailing keyvalues: one "solid" block per collision object, "ragdollconstraint" blocks, "collisionrules",
	// "editparams" - the same text CRagdollProp/RagdollCreateObjects parses with IVPhysicsKeyParser.
	TArray<FSourceKeyValues> Roots;
	if (TextOffset < Bytes.Num())
	{
		FString Error;
		TConstArrayView<uint8> Text(Bytes.GetData() + TextOffset, Bytes.Num() - TextOffset);
		if (!FSourceKeyValues::ParseBytes(Text, Roots, &Error))
		{
			if (OutError) { *OutError = FString::Printf(TEXT("collision keyvalues: %s"), *Error); }
			return false;
		}
	}

	for (const FSourceKeyValues& Block : Roots)
	{
		if (Block.Key.Equals(TEXT("solid"), ESearchCase::IgnoreCase))
		{
			const int32 Index = Block.GetInt(TEXT("index"), -1);
			if (!Solids.IsValidIndex(Index))
			{
				continue;
			}
			FSourcePHYSolid& Solid = Solids[Index];
			Solid.BoneName = Block.GetString(TEXT("name"));
			Solid.ParentSolidName = Block.GetString(TEXT("parent"));
			Solid.Mass = Block.GetFloat(TEXT("mass"), 1.0f);
			Solid.SurfaceProp = Block.GetString(TEXT("surfaceprop"));
			Solid.Damping = Block.GetFloat(TEXT("damping"), 0.0f);
			Solid.RotDamping = Block.GetFloat(TEXT("rotdamping"), 0.0f);
			Solid.Inertia = Block.GetFloat(TEXT("inertia"), 1.0f);
			Solid.Volume = Block.GetFloat(TEXT("volume"), 0.0f);
		}
		else if (Block.Key.Equals(TEXT("ragdollconstraint"), ESearchCase::IgnoreCase))
		{
			FSourcePHYConstraint& C = Constraints.AddDefaulted_GetRef();
			C.ParentSolid = Block.GetInt(TEXT("parent"), -1);
			C.ChildSolid = Block.GetInt(TEXT("child"), -1);
			C.Min = FVector(Block.GetFloat(TEXT("xmin")), Block.GetFloat(TEXT("ymin")), Block.GetFloat(TEXT("zmin")));
			C.Max = FVector(Block.GetFloat(TEXT("xmax")), Block.GetFloat(TEXT("ymax")), Block.GetFloat(TEXT("zmax")));
			C.Friction = FVector(Block.GetFloat(TEXT("xfriction")), Block.GetFloat(TEXT("yfriction")), Block.GetFloat(TEXT("zfriction")));
		}
		else if (Block.Key.Equals(TEXT("break"), ESearchCase::IgnoreCase))
		{
			// CBreakParser: one piece of the model, named without an extension and relative to models/.
			FSourcePHYBreak& Piece = Breaks.AddDefaulted_GetRef();
			Piece.ModelName = Block.GetString(TEXT("model"));
			if (Piece.ModelName.IsEmpty())
			{
				Piece.ModelName = Block.GetString(TEXT("ragdoll"));
				Piece.bIsRagdoll = !Piece.ModelName.IsEmpty();
			}
			FVector3f Offset = FVector3f::ZeroVector;
			FSourceCoords::ParseVector(Block.GetString(TEXT("offset")), Offset);
			Piece.Offset = FSourceCoords::ToUE(Offset, Scale);
			Piece.FadeTime = Block.GetFloat(TEXT("fadetime"), 0.0f);
			Piece.Health = Block.GetFloat(TEXT("health"), 1.0f);
			Piece.BurstScale = Block.GetFloat(TEXT("burst"), 0.0f);
			Piece.bMotionDisabled = Block.GetBool(TEXT("motiondisabled"), false);
			Piece.PlacementName = Block.GetString(TEXT("placementbone"));
			Piece.bPlacementIsBone = !Piece.PlacementName.IsEmpty();
			if (Piece.PlacementName.IsEmpty())
			{
				Piece.PlacementName = Block.GetString(TEXT("placementattachment"));
			}
			if (Piece.ModelName.IsEmpty())
			{
				Breaks.Pop();
			}
		}
		else if (Block.Key.Equals(TEXT("collisionrules"), ESearchCase::IgnoreCase))
		{
			bSelfCollisions = Block.GetBool(TEXT("selfcollisions"), false);
		}
	}
	return true;
}

float FSourcePHYFile::GetTotalMass() const
{
	float Total = 0.0f;
	for (const FSourcePHYSolid& Solid : Solids)
	{
		Total += Solid.Mass;
	}
	return Total;
}

int32 FSourcePHYFile::FindSolidByBone(const FString& BoneName) const
{
	for (int32 i = 0; i < Solids.Num(); ++i)
	{
		if (Solids[i].BoneName.Equals(BoneName, ESearchCase::IgnoreCase))
		{
			return i;
		}
	}
	return INDEX_NONE;
}

