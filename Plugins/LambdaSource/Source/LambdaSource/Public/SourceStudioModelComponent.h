#pragma once

#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"
#include "SourceMDLFile.h"
#include "SourceStudioModelComponent.generated.h"

class ULambdaMaterialLibrary;

/**
 * Fired when a sequence's cycle crosses an mstudioevent_t. Newer models name their events ("AE_HEADCRAB_JUMPATTACK",
 * "AE_CL_PLAYSOUND"); older ones only carry the numeric id.
 */
DECLARE_MULTICAST_DELEGATE_ThreeParams(FSourceAnimationEvent, int32 /*EventId*/, const FString& /*Name*/, const FString& /*Options*/);

/** What a ray found in a model's hitboxes. */
struct FSourceHitboxHit
{
	int32 Hitbox = INDEX_NONE;
	int32 Bone = INDEX_NONE;
	int32 Group = 0;			// HITGROUP_*
	FVector Point = FVector::ZeroVector;	// world
	float Distance = 0.0f;		// along the ray, cm
};

/**
 * Renders and animates a Source studio model (.mdl) at runtime.
 *
 * Source draws studio models with GPU skinning against a cooked vertex buffer; we have neither a cooked asset nor a
 * skeletal mesh here, so the reference-pose vertices are skinned on the CPU each frame and pushed into the
 * procedural mesh. That is affordable for a view model (a few thousand vertices) and keeps the whole path runtime -
 * a modder drops a .mdl in and it animates, with no editor import step.
 */
UCLASS(ClassGroup = (Lambda), meta = (BlueprintSpawnableComponent))
class LAMBDASOURCE_API USourceStudioModelComponent : public UProceduralMeshComponent
{
	GENERATED_BODY()

public:
	USourceStudioModelComponent(const FObjectInitializer& ObjectInitializer);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Loads a model ("models/weapons/v_pistol.mdl") and shows it in its bind pose. Materials may be null. */
	bool SetModel(const FString& RelativeModelPath, ULambdaMaterialLibrary* Materials);

	/** Drops the current model and clears the mesh. */
	void ClearModel();

	bool HasModel() const { return Model.IsValid() && Model->IsLoaded(); }
	const FSourceMDLFile* GetModel() const { return Model.Get(); }
	const FString& GetModelPath() const { return ModelPath; }

	/** CBaseCombatWeapon::SendWeaponAnim: picks a sequence for the activity and plays it from the start. */
	bool PlayActivity(const FString& ActivityName);
	/**
	 * SetSequence: the sequence that was playing keeps running for a moment and is blended out
	 * (CSequenceTransitioner), so activities do not snap. bInterpolate false snaps, as does STUDIO_SNAP.
	 */
	bool PlaySequence(int32 SequenceIndex, bool bInterpolate = true);

	int32 GetSequence() const { return CurrentSequence; }
	float GetCycle() const { return Cycle; }
	/** CBaseAnimating::SequenceDuration for the sequence now playing, in seconds. */
	float GetSequenceDuration() const;
	/** True once a non-looping sequence has run to its end. */
	bool IsSequenceFinished() const { return bSequenceFinished; }

	/** Multiplies the rate the cycle advances at (CBaseAnimating::m_flPlaybackRate). */
	void SetPlaybackRate(float Rate) { PlaybackRate = Rate; }

	/** Ground speed authored into the current sequence's root motion, in Source units/sec (0 if none). */
	float GetSequenceGroundSpeed() const;

	/** World-space transform of a named attachment ("muzzle", "shell_eject"), from the current pose. */
	bool GetAttachmentWorld(const FString& Name, FVector& OutLocation, FVector& OutForward) const;

	/** World transform of a bone in the pose on screen. */
	FTransform GetBoneWorldTransform(int32 Bone) const;

	// ---- Gestures (CBaseAnimatingOverlay::AddGesture): layered sequences played over the current one ----

	/** RestartGesture: plays the activity's sequence as a layer on top of the current sequence, restarting it if already playing. */
	bool PlayGesture(const FString& ActivityName);
	bool IsPlayingGesture(const FString& ActivityName) const;
	/** Duration of the sequence a gesture activity would play, seconds (0 if none). */
	float GetGestureDuration(const FString& ActivityName) const;

	// ---- Bodygroups ----
	/** SetBodygroup: rebuilds the mesh with that model of the body part. */
	bool SetBodygroup(int32 BodyPart, int32 Value);

	// ---- Hitboxes ----
	bool HasHitboxes() const;
	/** Tests a world-space ray against the posed hitboxes (what Source's bullets hit); nearest wins. */
	bool TraceHitboxes(const FVector& Start, const FVector& End, FSourceHitboxHit& OutHit) const;

	/** Bone-to-model transforms of the pose on screen, Source space. */
	const TArray<FSourceMatrix3x4>& GetBoneToModel() const { return BoneToModel; }

	/**
	 * Hands the pose to something else (a ragdoll): the given bone matrices are skinned and the sequence stops
	 * advancing until ClearExternalPose().
	 */
	void SetExternalPose(const TArray<FSourceMatrix3x4>& InBoneToModel);
	void ClearExternalPose() { bExternalPose = false; }

	/**
	 * Code-driven bones, for the barnacle's tongue: the bone's model-space position is replaced after the
	 * sequence pose is composed, keeping whatever rotation the animation gave it - the same trick
	 * C_NPC_Barnacle::BuildTransformations plays, spacing the tongue bones between the root and the tip.
	 * Positions are component-local, UE cm.
	 */
	void SetBonePositionOverride(const FString& BoneName, const FVector& ComponentPosition);
	void ClearBonePositionOverrides() { BonePositionOverrides.Reset(); }
	bool HasExternalPose() const { return bExternalPose; }

	FSourceAnimationEvent OnAnimationEvent;

private:
	/** Re-skins the mesh from the current pose and pushes it to the render thread. */
	void RefreshPose();
	/** Evaluates the current sequence, the sequences fading out behind it and the gesture layers, then refreshes. */
	void ComposePose();
	/** Animation LOD: whether this model has earned a new pose this frame (see the definition). */
	bool ShouldComposePoseThisFrame();
	/** Moves the animation on in game time without composing anything. */
	void AdvanceCycleOnly(float DeltaTime);
	float LastPoseTime = 0.0f;

	struct FGestureLayer
	{
		int32 Sequence = INDEX_NONE;
		FString Activity;
		float Cycle = 0.0f;
	};
	TArray<FGestureLayer> Gestures;

	/**
	 * One entry of CSequenceTransitioner's queue: a sequence that has been replaced, still running, its weight
	 * fading to zero. Ordered oldest first.
	 */
	struct FTransitionLayer
	{
		int32 Sequence = INDEX_NONE;
		float Cycle = 0.0f;
		float PlaybackRate = 1.0f;
		float Age = 0.0f;			// seconds since it was laid down (curtime - m_flLayerAnimtime)
		float FadeOutTime = 0.2f;	// m_flLayerFadeOuttime
		bool bLooping = false;
	};
	TArray<FTransitionLayer> Transitions;

	/** CAnimationLayer::GetFadeout: the layer's weight, 1 -> 0 over its fade time. */
	static float TransitionWeight(const FTransitionLayer& Layer);

	TSharedPtr<FSourceMDLFile> Model;
	FString ModelPath;

	/** Bone-to-model transforms for the pose currently displayed, in Source space. */
	TArray<FSourceMatrix3x4> BoneToModel;
	TMap<int32, FVector> BonePositionOverrides;

	int32 CurrentSequence = INDEX_NONE;
	float Cycle = 0.0f;
	float PlaybackRate = 1.0f;
	bool bSequenceLooping = false;
	bool bSequenceFinished = false;
	bool bExternalPose = false;

	UPROPERTY(Transient)
	TObjectPtr<ULambdaMaterialLibrary> MaterialLibrary;
};
