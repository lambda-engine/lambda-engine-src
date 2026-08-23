#pragma once

#include "CoreMinimal.h"

/**
 * One "solid" of a .phy: a vphysics collision object attached to a bone - its convex hulls (in bone space, UE
 * units and UE axes) plus the keyvalues studiomdl wrote for it ($collisionjoints: mass, damping, surfaceprop).
 */
/**
 * One "break" block from the .phy text section: a piece this model comes apart into (props_shared.cpp's
 * breakmodel_t, as read by CBreakParser). A crate names its own chunks this way.
 */
struct LAMBDASOURCE_API FSourcePHYBreak
{
	FString ModelName;				// "props_junk/wood_crate001a_Chunk01" (no extension)
	/** Where the piece sits relative to the prop, in UE cm/axes. */
	FVector Offset = FVector::ZeroVector;
	float FadeTime = 0.0f;			// seconds before the piece is taken away; 0 means it stays
	float Health = 1.0f;
	/** How hard the piece is thrown out from the middle when it is created. */
	float BurstScale = 0.0f;
	bool bIsRagdoll = false;
	bool bMotionDisabled = false;
	/** "placementbone"/"placementattachment": where on the parent the piece starts. */
	FString PlacementName;
	bool bPlacementIsBone = false;
};

struct LAMBDASOURCE_API FSourcePHYSolid
{
	int32 Index = 0;
	FString BoneName;				// "HeadcrabClassic.BodyControl"
	FString ParentSolidName;		// bone name of the parent solid, from the text section
	float Mass = 1.0f;				// kg
	FString SurfaceProp;			// "alienflesh"
	float Damping = 0.0f;
	float RotDamping = 0.0f;
	float Inertia = 1.0f;
	float Volume = 0.0f;
	/** Convex pieces (IVP "ledges"); a ragdoll solid normally has one. Points are bone-local, UE axes, cm. */
	TArray<TArray<FVector>> Hulls;
	/** IVP mass centre, bone-local, UE axes, cm. */
	FVector MassCenter = FVector::ZeroVector;
};

/** A "ragdollconstraint" block: joint limits between two solids, degrees about the parent's x, y, z. */
struct LAMBDASOURCE_API FSourcePHYConstraint
{
	int32 ParentSolid = INDEX_NONE;
	int32 ChildSolid = INDEX_NONE;
	FVector Min = FVector::ZeroVector;		// xmin, ymin, zmin
	FVector Max = FVector::ZeroVector;		// xmax, ymax, zmax
	FVector Friction = FVector::ZeroVector;	// xfriction, yfriction, zfriction
};

/**
 * Reader for Source's .phy collision models (phyfile.h + the Ipion "compact surface" format vphysics stores its
 * solids in, followed by the keyvalues text that names each solid's bone and lists the ragdoll joints).
 *
 * The binary part is documented nowhere in the SDK (vphysics is closed), so it is decoded from the layout the
 * community established: a compactsurfaceheader_t per solid, an IVP_Compact_Surface (mass centre, inertia, ledge
 * tree offset), then the ledges - each a convex piece with triangles that index into a shared point array of
 * IVP_U_Float_Hesse (x, y, z, hesse) in metres, y up. IVP space becomes Source space as (x, z, -y) / 0.0254.
 */
class LAMBDASOURCE_API FSourcePHYFile
{
public:
	/** Loads the .phy beside a model ("models/headcrabclassic.mdl" -> "models/headcrabclassic.phy"). */
	bool Load(const FString& RelativeModelPath, float Scale, FString* OutError = nullptr);

	bool IsLoaded() const { return bLoaded; }
	const TArray<FSourcePHYSolid>& GetSolids() const { return Solids; }
	/** BreakModelList: the pieces this model breaks into, in the order the .phy lists them. */
	const TArray<FSourcePHYBreak>& GetBreaks() const { return Breaks; }
	const TArray<FSourcePHYConstraint>& GetConstraints() const { return Constraints; }
	bool AllowsSelfCollisions() const { return bSelfCollisions; }
	float GetTotalMass() const;

	int32 FindSolidByBone(const FString& BoneName) const;

private:
	bool ParseBinary(const TArray<uint8>& Bytes, float Scale, int32& OutTextOffset, FString* OutError);
	bool ParseText(const TArray<uint8>& Bytes, int32 TextOffset, float Scale, FString* OutError);

	TArray<FSourcePHYSolid> Solids;
	TArray<FSourcePHYBreak> Breaks;
	TArray<FSourcePHYConstraint> Constraints;
	bool bSelfCollisions = false;
	bool bLoaded = false;
};
