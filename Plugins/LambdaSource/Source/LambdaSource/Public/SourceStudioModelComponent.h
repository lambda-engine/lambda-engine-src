#pragma once

#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"
#include "SourceMDLFile.h"
#include "SourceStudioModelComponent.generated.h"

class ULambdaMaterialLibrary;

/** Fired when a sequence's cycle crosses an mstudioevent_t. Event ids follow Source's AE_* / classic numbering. */
DECLARE_MULTICAST_DELEGATE_TwoParams(FSourceAnimationEvent, int32 /*EventId*/, const FString& /*Options*/);

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
	bool PlaySequence(int32 SequenceIndex);

	int32 GetSequence() const { return CurrentSequence; }
	float GetCycle() const { return Cycle; }
	/** CBaseAnimating::SequenceDuration for the sequence now playing, in seconds. */
	float GetSequenceDuration() const;
	/** True once a non-looping sequence has run to its end. */
	bool IsSequenceFinished() const { return bSequenceFinished; }

	/** Multiplies the rate the cycle advances at (CBaseAnimating::m_flPlaybackRate). */
	void SetPlaybackRate(float Rate) { PlaybackRate = Rate; }

	/** World-space transform of a named attachment ("muzzle", "shell_eject"), from the current pose. */
	bool GetAttachmentWorld(const FString& Name, FVector& OutLocation, FVector& OutForward) const;

	FSourceAnimationEvent OnAnimationEvent;

private:
	/** Re-skins the mesh from the current pose and pushes it to the render thread. */
	void RefreshPose();

	TSharedPtr<FSourceMDLFile> Model;
	FString ModelPath;

	/** Bone-to-model transforms for the pose currently displayed, in Source space. */
	TArray<FSourceMatrix3x4> BoneToModel;

	int32 CurrentSequence = INDEX_NONE;
	float Cycle = 0.0f;
	float PlaybackRate = 1.0f;
	bool bSequenceLooping = false;
	bool bSequenceFinished = false;

	UPROPERTY(Transient)
	TObjectPtr<ULambdaMaterialLibrary> MaterialLibrary;
};
