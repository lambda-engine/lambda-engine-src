#pragma once

#include "CoreMinimal.h"

/**
 * One "solid" of a .phy: a vphysics collision object attached to a bone - its convex hulls (in bone space, UE
 * units and UE axes) plus the keyvalues studiomdl wrote for it ($collisionjoints: mass, damping, surfaceprop).
 */
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
	const TArray<FSourcePHYConstraint>& GetConstraints() const { return Constraints; }
	bool AllowsSelfCollisions() const { return bSelfCollisions; }
	float GetTotalMass() const;

	int32 FindSolidByBone(const FString& BoneName) const;

private:
	bool ParseBinary(const TArray<uint8>& Bytes, float Scale, int32& OutTextOffset, FString* OutError);
	bool ParseText(const TArray<uint8>& Bytes, int32 TextOffset, FString* OutError);

	TArray<FSourcePHYSolid> Solids;
	TArray<FSourcePHYConstraint> Constraints;
	bool bSelfCollisions = false;
	bool bLoaded = false;
};
