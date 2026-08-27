#include "Rendering/SourceStudioModelComponent.h"
#include "Core/LambdaStats.h"
#include "Materials/LambdaMaterialLibrary.h"
#include "Core/LambdaSourceModule.h"
#include "Core/LambdaSourceSettings.h"
#include "World/SourceGeometryBuilder.h"
#include "Rendering/SourceSkeletalMesh.h"
#include "Core/SourceCoordinates.h"

USourceStudioModelComponent::USourceStudioModelComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;

	// A view model is drawn, never collided with; collision would also make every re-skin re-cook the mesh.
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetCastShadow(false);

	// Models are shot at and have to show it. A skinned component does not take decals by default, and
	// SpawnDecalAttached quietly returns nothing for a component that refuses them.
	bReceivesDecals = true;

}

void USourceStudioModelComponent::ApplySkeletalMesh()
{
	if (!HasModel())
	{
		return;
	}
	// One mesh per model, bodygroup selection and torso cut, shared by every instance: it carries no pose, so
	// there is nothing per-instance about it. The cut keys the cache - the shadow body and the legs are the
	// same model path, one whole and one cut, and they must not trade meshes.
	TSet<int32> HiddenBones;
	FString CacheKey = BodygroupKey;
	if (!HiddenSubtreeBone.IsEmpty())
	{
		const TArray<FSourceStudioBone>& CutBones = Model->GetBones();
		int32 Root = INDEX_NONE;
		for (int32 i = 0; i < CutBones.Num(); ++i)
		{
			if (CutBones[i].Name.Equals(HiddenSubtreeBone, ESearchCase::IgnoreCase))
			{
				Root = i;
				break;
			}
		}
		for (int32 i = 0; Root != INDEX_NONE && i < CutBones.Num(); ++i)
		{
			for (int32 P = i; P != INDEX_NONE; P = CutBones[P].Parent)
			{
				if (P == Root)
				{
					HiddenBones.Add(i);
					break;
				}
			}
		}
		if (HiddenBones.Num() > 0)
		{
			CacheKey += TEXT("|cut:") + HiddenSubtreeBone;
		}
	}
	USkeletalMesh* Mesh = FSourceSkeletalMesh::GetOrBuild(ModelPath, *Model, MaterialLibrary, CacheKey,
		HiddenBones.Num() > 0 ? &HiddenBones : nullptr);
	if (Mesh)
	{
		SetSkinnedAssetAndUpdate(Mesh, /*bReinitPose=*/ true);
		// Re-stated after the asset is on: models are shot at and have to show it, and a skinned component
		// does not take decals of its own accord - SpawnDecalAttached quietly returns nothing for one that
		// refuses them, which is what swallowed every bullet mark on an NPC.
		SetReceivesDecals(true);
	}
}

bool USourceStudioModelComponent::SetModel(const FString& RelativeModelPath, ULambdaMaterialLibrary* Materials)
{
	ClearModel();

	TSharedPtr<FSourceMDLFile> Loaded = MakeShared<FSourceMDLFile>();
	FString Error;
	if (!Loaded->Load(RelativeModelPath, ULambdaSourceSettings::Get().UnitScale, &Error))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Studio model '%s': %s"), *RelativeModelPath, *Error);
		return false;
	}

	Model = Loaded;
	ModelPath = RelativeModelPath;
	MaterialLibrary = Materials;

	ApplySkeletalMesh();

	// Show the bind pose until something asks for a sequence, so a model with no animation still renders.
	Model->EvaluateBindPose(BoneToModel);
	CurrentSequence = INDEX_NONE;
	Cycle = 0.0f;
	bSequenceFinished = false;
	bSequenceLooping = false;
	Gestures.Reset();
	Transitions.Reset();
	return true;
}

void USourceStudioModelComponent::ClearModel()
{
	SetSkinnedAssetAndUpdate(nullptr, true);
	Model.Reset();
	ModelPath.Reset();
	BoneToModel.Reset();
	CurrentSequence = INDEX_NONE;
	Cycle = 0.0f;
	bSequenceFinished = false;
	bSequenceLooping = false;
	Gestures.Reset();
	Transitions.Reset();
}

bool USourceStudioModelComponent::PlayActivity(const FString& ActivityName)
{
	if (!HasModel())
	{
		return false;
	}
	const int32 Sequence = Model->SelectWeightedSequence(ActivityName);
	if (Sequence == INDEX_NONE)
	{
		return false;
	}
	return PlaySequence(Sequence);
}

bool USourceStudioModelComponent::PlaySequence(int32 SequenceIndex, bool bInterpolate)
{
	if (!HasModel() || !Model->GetSequences().IsValidIndex(SequenceIndex))
	{
		return false;
	}

	// CSequenceTransitioner::CheckForSequenceChange: the sequence being replaced stays in the queue, running on
	// its own, and is blended over the new one with a weight that fades to zero - which is what keeps an NPC from
	// snapping between idle, walk and attack. A STUDIO_SNAP sequence clears the queue instead.
	const FSourceStudioSequence& NewSeq = Model->GetSequences()[SequenceIndex];
	if (!bInterpolate || (NewSeq.Flags & FSourceMDLFile::STUDIO_SNAP))
	{
		Transitions.Reset();
	}
	else if (CurrentSequence != INDEX_NONE && CurrentSequence != SequenceIndex)
	{
		FTransitionLayer& Layer = Transitions.AddDefaulted_GetRef();
		Layer.Sequence = CurrentSequence;
		Layer.Cycle = Cycle;
		Layer.PlaybackRate = PlaybackRate;
		Layer.Age = 0.0f;
		// Source blends over the outgoing sequence's fadeout time, floored at studiomdl's 0.2s default.
		Layer.FadeOutTime = FMath::Max(0.05f, Model->GetSequences()[CurrentSequence].FadeOutTime);
		Layer.bLooping = bSequenceLooping;
		// more than a couple of these at once is a slideshow of half-blended poses; keep the newest few
		while (Transitions.Num() > 3)
		{
			Transitions.RemoveAt(0);
		}
	}

	CurrentSequence = SequenceIndex;
	Cycle = 0.0f;
	bSequenceLooping = Model->IsSequenceLooping(SequenceIndex);
	bSequenceFinished = false;

	ComposePose();
	return true;
}

float USourceStudioModelComponent::TransitionWeight(const FTransitionLayer& Layer)
{
	// CAnimationLayer::GetFadeout: 1 -> 0 over the fade time, on a spline.
	if (Layer.FadeOutTime <= 0.0f)
	{
		return 0.0f;
	}
	const float S = 1.0f - Layer.Age / Layer.FadeOutTime;
	if (S <= 0.0f)
	{
		return 0.0f;
	}
	if (S >= 1.0f)
	{
		return 1.0f;
	}
	return 3.0f * S * S - 2.0f * S * S * S;	// SimpleSpline
}

void USourceStudioModelComponent::AdvanceCycleOnly(float DeltaTime)
{
	// FrameAdvance without the pose: the animation keeps its place in game time so that a model skipped for a
	// few frames comes back where it should be, not where it was. Animation events still fire - they drive
	// footsteps and attacks, which have to happen whether or not anyone is looking.
	if (CurrentSequence == INDEX_NONE)
	{
		return;
	}
	const float Duration = Model->GetSequenceDuration(CurrentSequence);
	if (Duration <= 0.0f)
	{
		return;
	}
	const float PrevCycle = Cycle;
	Cycle += (DeltaTime * PlaybackRate) / Duration;
	if (Cycle >= 1.0f)
	{
		if (bSequenceLooping)
		{
			Cycle = FMath::Fmod(Cycle, 1.0f);
		}
		else
		{
			Cycle = 1.0f;
			bSequenceFinished = true;
		}
	}
	if (OnAnimationEvent.IsBound())
	{
		TArray<const FSourceStudioEvent*> Events;
		Model->CollectEvents(CurrentSequence, PrevCycle, Cycle, bSequenceLooping, Events);
		for (const FSourceStudioEvent* Event : Events)
		{
			OnAnimationEvent.Broadcast(Event->Event, Event->Name, Event->Options);
		}
	}
}

bool USourceStudioModelComponent::ShouldComposePoseThisFrame()
{
	// Animation LOD. Posing a model means composing its bones, skinning every vertex on the CPU and handing the
	// whole vertex buffer to the renderer; on the HL:A models, twenty to thirty thousand triangles each, that is
	// most of a frame. A model on screen earns that every frame. One that is not - behind the player, in another
	// room, or only casting a shadow into view - gets it ten times a second instead, which is more than enough
	// for a shadow to stay honest and for anything asking where its bones are, at a tenth of the cost.
	//
	// The test is deliberately not IsVisible(), which only reports that the component was not switched off: an
	// NPC standing behind the player is "visible" by that measure and was paying full price every frame.
	// GetLastRenderTimeOnScreen, not WasRecentlyRendered: the latter counts a model rendered into a shadow pass
	// as rendered, and an NPC behind the player casting a shadow into view still answers yes to it - which is
	// why gating on it saved almost nothing here. What matters is whether the model itself was on screen.
	if (bAlwaysComposePose)
	{
		return true;
	}
	const UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.0f;
	if (Now - GetLastRenderTimeOnScreen() < 0.15f)
	{
		return true;
	}
	if (Now - LastPoseTime < 0.1f)
	{
		return false;
	}
	LastPoseTime = Now;
	return true;
}

void USourceStudioModelComponent::ComposePose()
{
	SCOPE_CYCLE_COUNTER(STAT_LambdaComposePose);
	if (!HasModel())
	{
		return;
	}
	// CalcPose: the base sequence, then every gesture layer accumulated on top (AccumulateLayers).
	FSourceLocalPose Pose;
	Model->InitLocalPose(Pose);
	if (CurrentSequence != INDEX_NONE)
	{
		Model->AccumulateSequence(Pose, CurrentSequence, Cycle, 1.0f);
	}
	// C_BaseAnimating::MaintainSequenceTransitions: the sequences on their way out are accumulated over the
	// current one, newest first, at their fade weight - so the pose starts as the old animation and slides to the
	// new one over the fade time.
	for (int32 i = Transitions.Num() - 1; i >= 0; --i)
	{
		const FTransitionLayer& Layer = Transitions[i];
		Model->AccumulateSequence(Pose, Layer.Sequence, Layer.Cycle, TransitionWeight(Layer));
	}
	for (const FGestureLayer& Gesture : Gestures)
	{
		Model->AccumulateSequence(Pose, Gesture.Sequence, Gesture.Cycle, 1.0f);
	}
	// Bones the code owns pose where they are told, and their children follow (SetBoneRotationOverride).
	for (const TPair<int32, FQuat4f>& Override : BoneRotationOverrides)
	{
		if (Pose.Quat.IsValidIndex(Override.Key))
		{
			Pose.Quat[Override.Key] = Override.Value;
		}
	}

	// The aim constraints, solved inside this frame's pose (SetBoneAimConstraint). Parents first - the list is
	// sorted by bone index and Source skeletons are parent-before-child - so an elbow is solved under the
	// shoulder this same pass just corrected.
	if (BoneAimConstraints.Num() > 0)
	{
		const TArray<FSourceStudioBone>& AimBones = Model->GetBones();
		auto PoseGlobal = [&AimBones, &Pose](int32 Bone)
		{
			FQuat4f Q = Pose.Quat[Bone];
			for (int32 P = AimBones[Bone].Parent; P != INDEX_NONE; P = AimBones[P].Parent)
			{
				Q = Pose.Quat[P] * Q;
			}
			return Q;
		};
		for (const FBoneAim& Aim : BoneAimConstraints)
		{
			if (!Pose.Quat.IsValidIndex(Aim.Bone))
			{
				continue;
			}
			const int32 Parent = AimBones[Aim.Bone].Parent;
			const FQuat4f ParentGlobal = Parent != INDEX_NONE ? PoseGlobal(Parent) : FQuat4f::Identity;
			const FQuat4f GlobalNow = ParentGlobal * Pose.Quat[Aim.Bone];
			const FVector3f DirNow = GlobalNow.RotateVector(Aim.Axis);
			const FQuat4f GlobalNew = FQuat4f::FindBetweenNormals(DirNow, Aim.Dir) * GlobalNow;
			Pose.Quat[Aim.Bone] = ParentGlobal.Inverse() * GlobalNew;
		}
	}
	Model->BuildBoneToModel(Pose, BoneToModel);

	// Bones the code owns keep the animation's rotation but go where they are told (the barnacle's tongue).
	if (BonePositionOverrides.Num() > 0)
	{
		const float Scale = ULambdaSourceSettings::Get().UnitScale;
		for (const TPair<int32, FVector>& Override : BonePositionOverrides)
		{
			if (BoneToModel.IsValidIndex(Override.Key))
			{
				FTransform T = BoneToModel[Override.Key].ToUETransform(Scale);
				T.SetTranslation(Override.Value);
				BoneToModel[Override.Key] = FSourceMatrix3x4::FromUETransform(T, Scale);
			}
		}
	}
	RefreshPose();
}

void USourceStudioModelComponent::SetBoneRotationOverride(const FString& BoneName, const FQuat4f& LocalRotation)
{
	if (!HasModel())
	{
		return;
	}
	const TArray<FSourceStudioBone>& Bones = Model->GetBones();
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		if (Bones[i].Name.Equals(BoneName, ESearchCase::IgnoreCase))
		{
			BoneRotationOverrides.Add(i, LocalRotation);
			return;
		}
	}
}

void USourceStudioModelComponent::SetBoneAimConstraint(const FString& BoneName, const FString& ChildName, const FVector3f& ModelSpaceDir)
{
	if (!HasModel())
	{
		return;
	}
	const TArray<FSourceStudioBone>& Bones = Model->GetBones();
	int32 Bone = INDEX_NONE, Child = INDEX_NONE;
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		if (Bones[i].Name.Equals(BoneName, ESearchCase::IgnoreCase)) { Bone = i; }
		if (Bones[i].Name.Equals(ChildName, ESearchCase::IgnoreCase)) { Child = i; }
	}
	if (Bone == INDEX_NONE || Child == INDEX_NONE)
	{
		return;
	}

	// The axis that points at the child, in the bone's own space: fixed by the bind pose, true in any pose.
	TArray<FQuat4f> GlobalQuat;
	TArray<FVector3f> GlobalPos;
	GlobalQuat.SetNum(Bones.Num());
	GlobalPos.SetNum(Bones.Num());
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		const int32 P = Bones[i].Parent;
		GlobalQuat[i] = P != INDEX_NONE ? GlobalQuat[P] * Bones[i].Quat : Bones[i].Quat;
		GlobalPos[i] = P != INDEX_NONE ? GlobalPos[P] + GlobalQuat[P].RotateVector(Bones[i].Pos) : Bones[i].Pos;
	}

	FBoneAim Aim;
	Aim.Bone = Bone;
	Aim.Axis = GlobalQuat[Bone].Inverse().RotateVector((GlobalPos[Child] - GlobalPos[Bone]).GetSafeNormal());
	Aim.Dir = ModelSpaceDir.GetSafeNormal();
	BoneAimConstraints.RemoveAll([Bone](const FBoneAim& A) { return A.Bone == Bone; });
	BoneAimConstraints.Add(Aim);
	BoneAimConstraints.Sort([](const FBoneAim& A, const FBoneAim& B) { return A.Bone < B.Bone; });
}

void USourceStudioModelComponent::SetBonePositionOverride(const FString& BoneName, const FVector& ComponentPosition)
{
	if (!HasModel())
	{
		return;
	}
	const TArray<FSourceStudioBone>& Bones = Model->GetBones();
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		if (Bones[i].Name.Equals(BoneName, ESearchCase::IgnoreCase))
		{
			BonePositionOverrides.Add(i, ComponentPosition);
			return;
		}
	}
}

bool USourceStudioModelComponent::PlayGesture(const FString& ActivityName)
{
	if (!HasModel())
	{
		return false;
	}
	const int32 Sequence = Model->SelectWeightedSequence(ActivityName);
	if (Sequence == INDEX_NONE)
	{
		return false;
	}
	for (FGestureLayer& Gesture : Gestures)
	{
		if (Gesture.Activity.Equals(ActivityName, ESearchCase::IgnoreCase))
		{
			Gesture.Sequence = Sequence;
			Gesture.Cycle = 0.0f;
			return true;
		}
	}
	FGestureLayer& Gesture = Gestures.AddDefaulted_GetRef();
	Gesture.Sequence = Sequence;
	Gesture.Activity = ActivityName;
	Gesture.Cycle = 0.0f;
	return true;
}

bool USourceStudioModelComponent::IsPlayingGesture(const FString& ActivityName) const
{
	for (const FGestureLayer& Gesture : Gestures)
	{
		if (Gesture.Activity.Equals(ActivityName, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

float USourceStudioModelComponent::GetGestureDuration(const FString& ActivityName) const
{
	if (!HasModel())
	{
		return 0.0f;
	}
	const int32 Sequence = Model->SelectWeightedSequence(ActivityName);
	return Sequence != INDEX_NONE ? Model->GetSequenceDuration(Sequence) : 0.0f;
}

bool USourceStudioModelComponent::SetBodygroup(int32 BodyPart, int32 Value)
{
	if (!HasModel() || !Model->SetBodygroup(BodyPart, Value))
	{
		return false;
	}
	BodygroupKey = FString::Printf(TEXT("%s.%d=%d"), *BodygroupKey, BodyPart, Value);
	ApplySkeletalMesh();
	RefreshPose();
	return true;
}

bool USourceStudioModelComponent::HasHitboxes() const
{
	return HasModel() && Model->GetHitboxes().Num() > 0;
}

FTransform USourceStudioModelComponent::GetBoneWorldTransform(int32 Bone) const
{
	if (!BoneToModel.IsValidIndex(Bone))
	{
		return GetComponentTransform();
	}
	return BoneToModel[Bone].ToUETransform(ULambdaSourceSettings::Get().UnitScale) * GetComponentTransform();
}

bool USourceStudioModelComponent::TraceHitboxes(const FVector& Start, const FVector& End, FSourceHitboxHit& OutHit) const
{
	// TraceToStudio: each hitbox is an axis-aligned box in its bone's space; the ray is taken into that space and
	// slab-tested. Hitboxes are authored in Source units on Source axes (x forward, y left, z up), so the point in
	// UE bone space is mirrored and scaled back before the test.
	if (!HasHitboxes())
	{
		return false;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	bool bHit = false;
	float Best = TNumericLimits<float>::Max();
	const TArray<FSourceStudioHitbox>& Hitboxes = Model->GetHitboxes();
	for (int32 h = 0; h < Hitboxes.Num(); ++h)
	{
		const FSourceStudioHitbox& Box = Hitboxes[h];
		if (!BoneToModel.IsValidIndex(Box.Bone))
		{
			continue;
		}
		const FTransform BoneWorld = GetBoneWorldTransform(Box.Bone);
		const FVector LocalStartUE = BoneWorld.InverseTransformPosition(Start);
		const FVector LocalEndUE = BoneWorld.InverseTransformPosition(End);
		const FVector3f S = FSourceCoords::ToSource(LocalStartUE, Scale);
		const FVector3f E = FSourceCoords::ToSource(LocalEndUE, Scale);
		const FVector3f D = E - S;

		float TMin = 0.0f, TMax = 1.0f;
		bool bMiss = false;
		for (int32 a = 0; a < 3 && !bMiss; ++a)
		{
			if (FMath::Abs(D[a]) < KINDA_SMALL_NUMBER)
			{
				if (S[a] < Box.Min[a] || S[a] > Box.Max[a]) { bMiss = true; }
			}
			else
			{
				float T1 = (Box.Min[a] - S[a]) / D[a];
				float T2 = (Box.Max[a] - S[a]) / D[a];
				if (T1 > T2) { Swap(T1, T2); }
				TMin = FMath::Max(TMin, T1);
				TMax = FMath::Min(TMax, T2);
				if (TMin > TMax) { bMiss = true; }
			}
		}
		if (bMiss)
		{
			continue;
		}
		const float Dist = TMin * (End - Start).Size();
		if (Dist < Best)
		{
			Best = Dist;
			bHit = true;
			OutHit.Hitbox = h;
			OutHit.Bone = Box.Bone;
			OutHit.Group = Box.Group;
			OutHit.Point = Start + (End - Start) * TMin;
			OutHit.Distance = Dist;
		}
	}
	return bHit;
}

float USourceStudioModelComponent::GetSequenceDuration() const
{
	return HasModel() ? Model->GetSequenceDuration(CurrentSequence) : 0.0f;
}

float USourceStudioModelComponent::GetSequenceGroundSpeed() const
{
	return HasModel() ? Model->GetSequenceGroundSpeed(CurrentSequence) : 0.0f;
}

void USourceStudioModelComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	SCOPE_CYCLE_COUNTER(STAT_LambdaStudioTick);

	if (!HasModel() || !IsVisible() || bExternalPose)
	{
		return;
	}
	if (!ShouldComposePoseThisFrame())
	{
		// The cycle still advances - the animation keeps its place in game time, it is only the pose that waits.
		AdvanceCycleOnly(DeltaTime);
		return;
	}

	// The transition layers keep playing (CSequenceTransitioner::UpdateCurrent) and expire once faded out.
	bool bGesturesChanged = false;
	for (int32 i = Transitions.Num() - 1; i >= 0; --i)
	{
		FTransitionLayer& Layer = Transitions[i];
		Layer.Age += DeltaTime;
		const float LayerDuration = Model->GetSequenceDuration(Layer.Sequence);
		if (LayerDuration > 0.0f)
		{
			Layer.Cycle += (DeltaTime * Layer.PlaybackRate) / LayerDuration;
			Layer.Cycle = Layer.bLooping ? FMath::Fmod(Layer.Cycle, 1.0f) : FMath::Min(Layer.Cycle, 1.0f);
		}
		if (TransitionWeight(Layer) <= 0.0f)
		{
			Transitions.RemoveAt(i);
		}
		bGesturesChanged = true;	// the pose changes while anything is fading
	}

	for (int32 g = Gestures.Num() - 1; g >= 0; --g)
	{
		FGestureLayer& Gesture = Gestures[g];
		const float GestureDuration = Model->GetSequenceDuration(Gesture.Sequence);
		if (GestureDuration <= 0.0f)
		{
			Gestures.RemoveAt(g);
			bGesturesChanged = true;
			continue;
		}
		Gesture.Cycle += (DeltaTime * PlaybackRate) / GestureDuration;
		if (Gesture.Cycle >= 1.0f)
		{
			Gestures.RemoveAt(g);
		}
		bGesturesChanged = true;
	}

	if (CurrentSequence == INDEX_NONE)
	{
		if (bGesturesChanged)
		{
			ComposePose();
		}
		return;
	}

	const float Duration = Model->GetSequenceDuration(CurrentSequence);
	if (Duration <= 0.0f)
	{
		if (bGesturesChanged)
		{
			ComposePose();
		}
		return;
	}

	// C_BaseAnimating::FrameAdvance: the cycle advances by dt / duration, looping or clamping at the end.
	const float PrevCycle = Cycle;
	Cycle += (DeltaTime * PlaybackRate) / Duration;
	if (Cycle >= 1.0f)
	{
		if (bSequenceLooping)
		{
			Cycle = FMath::Fmod(Cycle, 1.0f);
		}
		else
		{
			Cycle = 1.0f;
			bSequenceFinished = true;
		}
	}

	if (OnAnimationEvent.IsBound())
	{
		TArray<const FSourceStudioEvent*> Events;
		Model->CollectEvents(CurrentSequence, PrevCycle, Cycle, bSequenceLooping, Events);
		for (const FSourceStudioEvent* Event : Events)
		{
			OnAnimationEvent.Broadcast(Event->Event, Event->Name, Event->Options);
		}
	}

	if (!FMath::IsNearlyEqual(PrevCycle, Cycle) || bGesturesChanged)
	{
		ComposePose();
	}
}

void USourceStudioModelComponent::SetExternalPose(const TArray<FSourceMatrix3x4>& InBoneToModel)
{
	if (!HasModel() || InBoneToModel.Num() != Model->GetNumBones())
	{
		return;
	}
	bExternalPose = true;
	BoneToModel = InBoneToModel;
	RefreshPose();
}

FBoxSphereBounds USourceStudioModelComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (!HasModel() || BoneToModel.Num() == 0)
	{
		return Super::CalcBounds(LocalToWorld);
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;

	// Every bone the model currently has, padded by how far its skin can sit from a bone. Source gives that
	// distance directly: the model's hull is what studiomdl measured the geometry to fit inside.
	FBox Box(ForceInit);
	for (const FSourceMatrix3x4& Bone : BoneToModel)
	{
		Box += Bone.ToUETransform(Scale).GetLocation();
	}
	const FVector3f HullSize = Model->GetHullMax() - Model->GetHullMin();
	const float Padding = FMath::Max(HullSize.GetAbsMax() * 0.5f, 1.0f) * Scale;
	return FBoxSphereBounds(Box.ExpandBy(Padding)).TransformBy(LocalToWorld);
}

void USourceStudioModelComponent::RefreshPose()
{
	if (!HasModel())
	{
		return;
	}

	SCOPE_CYCLE_COUNTER(STAT_LambdaMeshUpload);

	// Hand the bones over and let the GPU move the vertices. BoneToModel is in model space, which is what the
	// rest of the engine asks this component for (hitboxes, attachments); a posable mesh wants each bone
	// relative to its parent, so that conversion happens here and nowhere else.
	USkinnedAsset* Asset = GetSkinnedAsset();
	if (!Asset)
	{
		return;
	}
	const FReferenceSkeleton& RefSkeleton = Asset->GetRefSkeleton();
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const int32 NumBones = FMath::Min(BoneSpaceTransforms.Num(), BoneToModel.Num());
	for (int32 i = 0; i < NumBones; ++i)
	{
		const FTransform ComponentSpace = BoneToModel[i].ToUETransform(Scale);
		const int32 Parent = RefSkeleton.GetParentIndex(i);
		BoneSpaceTransforms[i] = (Parent == INDEX_NONE || Parent >= BoneToModel.Num())
			? ComponentSpace
			: ComponentSpace * BoneToModel[Parent].ToUETransform(Scale).Inverse();
		BoneSpaceTransforms[i].NormalizeRotation();
	}
	MarkRefreshTransformDirty();
	// The bounds are built from the bones, so they are stale the moment the pose moves.
	UpdateBounds();
	MarkRenderTransformDirty();
}

bool USourceStudioModelComponent::GetAttachmentWorld(const FString& Name, FVector& OutLocation, FVector& OutForward) const
{
	FVector LocalPos, LocalForward;
	if (!HasModel() || !Model->GetAttachment(Name, BoneToModel, LocalPos, LocalForward))
	{
		return false;
	}
	const FTransform& ToWorld = GetComponentTransform();
	OutLocation = ToWorld.TransformPosition(LocalPos);
	OutForward = ToWorld.TransformVectorNoScale(LocalForward).GetSafeNormal();
	return true;
}
