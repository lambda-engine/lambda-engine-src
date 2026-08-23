#pragma once

#include "CoreMinimal.h"
#include "SourceGeometryBuilder.h"	// FSourceMeshSection

/**
 * Source's matrix3x4_t. Stored row-major and applied as out = M * in (column-vector convention), which is the
 * opposite of UE's row-vector FMatrix, so bone maths is kept in this type and only the final result is converted.
 */
struct LAMBDASOURCE_API FSourceMatrix3x4
{
	float M[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };

	static FSourceMatrix3x4 FromQuatPos(const FQuat4f& Q, const FVector3f& P);

	FVector3f TransformPosition(const FVector3f& V) const;
	FVector3f TransformVector(const FVector3f& V) const;

	/** Returns this * Other, i.e. Other applied first. */
	FSourceMatrix3x4 Concat(const FSourceMatrix3x4& Other) const;

	FVector3f GetOrigin() const { return FVector3f(M[0][3], M[1][3], M[2][3]); }
	FVector3f GetForward() const { return FVector3f(M[0][0], M[1][0], M[2][0]); }
	FVector3f GetLeft() const { return FVector3f(M[0][1], M[1][1], M[2][1]); }
	FVector3f GetUp() const { return FVector3f(M[0][2], M[1][2], M[2][2]); }
};

/** mstudiobone_t: the bind pose plus the compression scales the RLE animation data is expressed in. */
struct LAMBDASOURCE_API FSourceStudioBone
{
	FString Name;
	int32 Parent = INDEX_NONE;
	FVector3f Pos = FVector3f::ZeroVector;
	FQuat4f Quat = FQuat4f::Identity;
	FVector3f Rot = FVector3f::ZeroVector;			// RadianEuler the animation deltas are added to
	FVector3f PosScale = FVector3f::ZeroVector;
	FVector3f RotScale = FVector3f::ZeroVector;
	FSourceMatrix3x4 PoseToBone;
	FQuat4f Alignment = FQuat4f::Identity;
	int32 Flags = 0;
};

/** mstudiomovement_t: piecewise root motion of an animation, which is where an NPC's ground speed comes from. */
struct LAMBDASOURCE_API FSourceStudioMovement
{
	int32 EndFrame = 0;
	int32 MotionFlags = 0;
	FVector3f Position = FVector3f::ZeroVector;	// displacement over this block, in Source units
};

/** mstudioanimdesc_t. */
struct LAMBDASOURCE_API FSourceStudioAnimDesc
{
	FString Name;
	float Fps = 30.0f;
	int32 Flags = 0;
	int32 NumFrames = 1;
	int32 AnimBlock = 0;
	int32 AnimIndex = 0;
	int32 SectionIndex = 0;
	int32 SectionFrames = 0;
	int64 FileOffset = 0;		// every index above is relative to the animdesc itself

	/** mstudioanimsections_t: (animblock, animindex) per section when SectionFrames != 0. */
	TArray<TPair<int32, int32>> Sections;
	TArray<FSourceStudioMovement> Movements;
};

/** mstudioevent_t: a cue fired at a point in a sequence's cycle (muzzle flash, shell eject, sound). */
struct LAMBDASOURCE_API FSourceStudioEvent
{
	float Cycle = 0.0f;
	int32 Event = 0;
	FString Name;			// non-empty on newer models; the numeric Event is the fallback
	FString Options;
};

/** mstudioseqdesc_t. Only the single-blend case is decoded; blends need pose parameters we do not drive. */
struct LAMBDASOURCE_API FSourceStudioSequence
{
	FString Label;
	FString ActivityName;	// e.g. "ACT_VM_PRIMARYATTACK"; empty when the sequence has no activity
	int32 Flags = 0;
	int32 ActivityWeight = 0;
	int32 NumBlends = 0;
	int32 AnimDescIndex = INDEX_NONE;
	TArray<FSourceStudioEvent> Events;
};

/** mstudioattachment_t: a named frame parented to a bone ("muzzle", "shell_eject", ...). */
struct LAMBDASOURCE_API FSourceStudioAttachment
{
	FString Name;
	int32 Bone = INDEX_NONE;
	FSourceMatrix3x4 Local;
};

/** Per-vertex data needed to re-skin a section, kept in Source space so bone maths stays in Source's convention. */
struct FSourceSkinVertex
{
	FVector3f Position = FVector3f::ZeroVector;
	FVector3f Normal = FVector3f::ZeroVector;
	uint8 NumBones = 0;
	uint8 Bones[3] = { 0, 0, 0 };
	float Weights[3] = { 0, 0, 0 };
};

/**
 * Reader for Source studio models: the .mdl (header, bones, sequences, materials, body parts), the .vvd (vertex
 * data) and the .vtx (hardware-optimised index data). A model needs all three, and Source checks that their
 * checksums agree.
 *
 * Load() builds the reference pose. EvaluateSequence() + ApplyPose() then animate it: the RLE-compressed bone
 * tracks are decoded exactly as bonesetup/bone_decode.cpp does, and vertices are skinned on the CPU because the
 * geometry lives in a UProceduralMeshComponent rather than a cooked skeletal mesh.
 */
class LAMBDASOURCE_API FSourceMDLFile
{
public:
	static constexpr int32 MDL_IDENT = 'TSDI';	// "IDST"
	static constexpr int32 VVD_IDENT = 'VSDI';	// "IDSV"

	/** studio.h sequence/animation flags. */
	static constexpr int32 STUDIO_LOOPING = 0x0001;

	/** Loads "models/weapons/v_pistol.mdl" through the virtual file system, plus its .vvd and best .vtx. */
	bool Load(const FString& RelativeModelPath, float Scale, FString* OutError = nullptr);

	bool IsLoaded() const { return bLoaded; }
	const FString& GetModelName() const { return ModelName; }
	int32 GetVersion() const { return Version; }
	int32 GetNumBones() const { return Bones.Num(); }

	/** Mesh sections in UE units and UE space, one per material, ready for a procedural mesh component. */
	TArray<FSourceMeshSection> Sections;

	const TArray<FSourceStudioBone>& GetBones() const { return Bones; }
	const TArray<FSourceStudioSequence>& GetSequences() const { return Sequences; }
	const TArray<FSourceStudioAnimDesc>& GetAnimDescs() const { return AnimDescs; }
	const TArray<FSourceStudioAttachment>& GetAttachments() const { return Attachments; }

	/** Total triangles across all sections. */
	int32 GetNumTriangles() const;

	// ---- Sequences ----

	/** CStudioHdr::SelectWeightedSequence: picks among the sequences with this activity, weighted by actweight. */
	int32 SelectWeightedSequence(const FString& ActivityName) const;
	int32 FindSequenceByLabel(const FString& Label) const;

	/** Seconds the sequence runs for: Studio_Duration = (numframes - 1) / fps. */
	float GetSequenceDuration(int32 SequenceIndex) const;
	bool IsSequenceLooping(int32 SequenceIndex) const;

	/** GetSequenceGroundSpeed: the animation's root-motion distance over its duration, in Source units/sec. */
	float GetSequenceGroundSpeed(int32 SequenceIndex) const;

	/** $surfaceprop of the model ("alienflesh", "metal"...), from studiohdr_t::surfacepropindex. */
	const FString& GetSurfaceProp() const { return SurfaceProp; }

	/**
	 * Decodes one sequence at a normalised cycle (0..1) into bone-to-model transforms, in Source space.
	 * Bones the animation does not touch keep their bind pose, exactly as InitPose/CalcAnimation leave them.
	 */
	bool EvaluateSequence(int32 SequenceIndex, float Cycle, TArray<FSourceMatrix3x4>& OutBoneToModel) const;

	/** Fills OutBoneToModel with the bind pose (what a model with no sequences renders as). */
	void EvaluateBindPose(TArray<FSourceMatrix3x4>& OutBoneToModel) const;

	/** Skins Sections in place from a bone set produced by EvaluateSequence. */
	void ApplyPose(const TArray<FSourceMatrix3x4>& BoneToModel);

	/** Resolves a named attachment to a position and forward direction in UE space, relative to the model origin. */
	bool GetAttachment(const FString& Name, const TArray<FSourceMatrix3x4>& BoneToModel,
		FVector& OutPosition, FVector& OutForward) const;

	/** Events in (PrevCycle, NewCycle], handling wrap-around for looping sequences. */
	void CollectEvents(int32 SequenceIndex, float PrevCycle, float NewCycle, bool bLooping,
		TArray<const FSourceStudioEvent*>& OutEvents) const;

private:
	/** Picks the best available .vtx for a model path (Source tries dx90, then dx80, then software). */
	bool ReadVtx(const FString& BasePath, TArray<uint8>& OutData, FString& OutUsedPath) const;

	void ReadBones(const TArray<uint8>& Mdl);
	void ReadSequences(const TArray<uint8>& Mdl);
	void ReadAttachments(const TArray<uint8>& Mdl);

	FString ModelName;
	int32 Version = 0;
	float UnitScale = 1.0f;
	bool bLoaded = false;

	TArray<FSourceStudioBone> Bones;
	TArray<FSourceStudioAnimDesc> AnimDescs;
	TArray<FSourceStudioSequence> Sequences;
	TArray<FSourceStudioAttachment> Attachments;

	/** Per-section skinning input, parallel to Sections. Empty when the model has no bones worth skinning. */
	TArray<TArray<FSourceSkinVertex>> SkinVertices;

	/** The .mdl is kept so animation data can be decoded on demand rather than unpacked for every frame up front. */
	TArray<uint8> MdlData;

	/**
	 * The external animation file (studiohdr_t::szanimblocknameindex, "models/x.ani"). studiomdl moves most of
	 * a character's animation out of the .mdl into numbered blocks here; mstudioanimblock_t gives each block's
	 * byte range in this file, and an animdesc addresses its data as block + offset.
	 */
	TArray<uint8> AniData;
	FString AnimBlockName;
	TArray<TPair<int32, int32>> AnimBlocks;	// (datastart, dataend) per block; block 0 is "in the .mdl"
	FString SurfaceProp;
};
