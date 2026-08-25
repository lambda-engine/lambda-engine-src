#pragma once

#include "CoreMinimal.h"
#include "World/SourceGeometryBuilder.h"	// FSourceMeshSection

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

	/**
	 * As a UE transform in the model's space (cm, UE axes): the y -> -y mirror applied on both sides, so
	 * X = forward, Y = -left, Z = up. The inverse round-trips.
	 */
	FTransform ToUETransform(float Scale) const;
	static FSourceMatrix3x4 FromUETransform(const FTransform& T, float Scale);
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

/** mstudioautolayer_t: a sequence blended in over part of the parent's cycle (how studiomdl builds gestures). */
struct LAMBDASOURCE_API FSourceStudioAutoLayer
{
	int32 Sequence = INDEX_NONE;
	int32 Pose = INDEX_NONE;
	int32 Flags = 0;		// STUDIO_AL_*
	float Start = 0.0f;
	float Peak = 0.0f;
	float Tail = 0.0f;
	float End = 0.0f;
};

/** mstudioseqdesc_t. Only the single-blend case is decoded; blends need pose parameters we do not drive. */
struct LAMBDASOURCE_API FSourceStudioSequence
{
	FString Label;
	FString ActivityName;	// e.g. "ACT_VM_PRIMARYATTACK"; empty when the sequence has no activity
	int32 Flags = 0;		// STUDIO_LOOPING, STUDIO_DELTA, STUDIO_POST, ...
	int32 ActivityWeight = 0;
	int32 NumBlends = 0;
	int32 AnimDescIndex = INDEX_NONE;
	/** fadeintime / fadeouttime: how long studiomdl says a transition into or out of this sequence should take. */
	float FadeInTime = 0.2f;
	float FadeOutTime = 0.2f;
	TArray<FSourceStudioEvent> Events;
	TArray<FSourceStudioAutoLayer> AutoLayers;
	/** Per-bone blend weights (the $weightlist); empty means 1 everywhere. */
	TArray<float> BoneWeights;
};

/** mstudiobbox_t: a hitbox - an oriented box on a bone with a hit group (HITGROUP_HEAD, _CHEST, ...). */
struct LAMBDASOURCE_API FSourceStudioHitbox
{
	int32 Bone = INDEX_NONE;
	int32 Group = 0;
	FVector3f Min = FVector3f::ZeroVector;	// bone space, Source units
	FVector3f Max = FVector3f::ZeroVector;
	FString Name;
};

/** A pose as per-bone local rotations and positions (Source's q[] / pos[] arrays), before the hierarchy is applied. */
struct LAMBDASOURCE_API FSourceLocalPose
{
	TArray<FQuat4f> Quat;
	TArray<FVector3f> Pos;
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
	/** STUDIO_SNAP: this sequence is not blended into - it snaps (CSequenceTransitioner). */
	static constexpr int32 STUDIO_SNAP = 0x0002;

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

	/**
	 * studiohdr_t::hull_min / hull_max: the box the model claims to occupy, in Source units. studiomdl writes
	 * either the QC's $bbox or, without one, how far the compiled animations actually throw the model about.
	 */
	const FVector3f& GetHullMin() const { return HullMin; }
	const FVector3f& GetHullMax() const { return HullMax; }

	/** $surfaceprop of the model ("alienflesh", "metal"...), from studiohdr_t::surfacepropindex. */
	const FString& GetSurfaceProp() const { return SurfaceProp; }
	/** studiohdr_t::KeyValueText: the model's own keyvalues, which is where a prop's prop_data section lives. */
	const FString& GetKeyValueText() const { return KeyValueText; }

	/**
	 * Decodes one sequence at a normalised cycle (0..1) into bone-to-model transforms, in Source space.
	 * Bones the animation does not touch keep their bind pose, exactly as InitPose/CalcAnimation leave them.
	 */
	bool EvaluateSequence(int32 SequenceIndex, float Cycle, TArray<FSourceMatrix3x4>& OutBoneToModel) const;

	/** Fills OutBoneToModel with the bind pose (what a model with no sequences renders as). */
	void EvaluateBindPose(TArray<FSourceMatrix3x4>& OutBoneToModel) const;

	/** The reference-pose vertices and their bone influences, per section - what a skinned mesh is built from. */
	const TArray<TArray<FSourceSkinVertex>>& GetSkinVertices() const { return SkinVertices; }

	// ---- Layered posing (bone_setup.cpp): a base sequence plus gestures accumulated on top ----

	/** InitPose: every bone at its bind pose. */
	void InitLocalPose(FSourceLocalPose& Pose) const;
	/**
	 * AccumulatePose: blends (or, for delta sequences, adds) one sequence at a cycle into Pose with a weight,
	 * including the sequence's autolayers - which is how a flinch gesture lands on top of a walk.
	 */
	bool AccumulateSequence(FSourceLocalPose& Pose, int32 SequenceIndex, float Cycle, float Weight) const;
	/** Local rotations/positions -> bone-to-model matrices through the hierarchy. */
	void BuildBoneToModel(const FSourceLocalPose& Pose, TArray<FSourceMatrix3x4>& OutBoneToModel) const;
	/**
	 * Whether the sequence adds to the pose under it rather than replacing it. studiomdl marks the animation
	 * delta when a $sequence subtracts a reference pose, and does not always mark the sequence with it, so both
	 * are consulted.
	 */
	bool IsSequenceDelta(int32 SequenceIndex) const;

	// ---- Hitboxes (the default hitbox set) ----
	const TArray<FSourceStudioHitbox>& GetHitboxes() const { return Hitboxes; }

	// ---- Bodygroups: which model each body part shows (SetBodygroup) ----
	int32 GetNumBodyParts() const { return BodyPartNames.Num(); }
	const FString& GetBodyPartName(int32 BodyPart) const { return BodyPartNames[BodyPart]; }
	int32 FindBodyPart(const FString& Name) const;
	int32 GetBodyPartNumModels(int32 BodyPart) const { return BodyPartModelCounts.IsValidIndex(BodyPart) ? BodyPartModelCounts[BodyPart] : 0; }
	int32 GetBodygroup(int32 BodyPart) const { return BodygroupValues.IsValidIndex(BodyPart) ? BodygroupValues[BodyPart] : 0; }
	/** Rebuilds Sections for the new choice; the owner must push the sections to its mesh again. */
	bool SetBodygroup(int32 BodyPart, int32 Value);

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
	void ReadHitboxes(const TArray<uint8>& Mdl);
	/** Builds Sections/SkinVertices from the .mdl/.vvd/.vtx for the current bodygroup choices. */
	void BuildSections();

	/** CalcPoseSingle: one sequence's own animation (no layers) as local bone transforms. */
	bool CalcPoseSingle(int32 SequenceIndex, float Cycle, FSourceLocalPose& Out) const;
	/** AddSequenceLayers: the sequence's autolayers accumulated into Pose. */
	void AddSequenceLayers(FSourceLocalPose& Pose, int32 SequenceIndex, float Cycle, float Weight) const;
	/** SlerpBones: blends (or adds, for delta sequences) Layer into Pose by Weight and the sequence's bone weights. */
	void SlerpBones(FSourceLocalPose& Pose, const FSourceStudioSequence& Seq, const FSourceLocalPose& Layer, float Weight, bool bDelta) const;

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
	TArray<uint8> VvdData;
	TArray<uint8> VtxData;
	FString VtxPath;
	TArray<FSourceStudioHitbox> Hitboxes;
	TArray<FString> BodyPartNames;
	TArray<int32> BodyPartModelCounts;
	TArray<int32> BodygroupValues;

	/**
	 * The external animation file (studiohdr_t::szanimblocknameindex, "models/x.ani"). studiomdl moves most of
	 * a character's animation out of the .mdl into numbered blocks here; mstudioanimblock_t gives each block's
	 * byte range in this file, and an animdesc addresses its data as block + offset.
	 */
	TArray<uint8> AniData;
	FString AnimBlockName;
	TArray<TPair<int32, int32>> AnimBlocks;	// (datastart, dataend) per block; block 0 is "in the .mdl"
	FString SurfaceProp;
	FString KeyValueText;
	FVector3f HullMin = FVector3f::ZeroVector;
	FVector3f HullMax = FVector3f::ZeroVector;
};
