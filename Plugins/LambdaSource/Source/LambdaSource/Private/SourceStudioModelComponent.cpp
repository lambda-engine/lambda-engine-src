#include "SourceStudioModelComponent.h"
#include "LambdaMaterialLibrary.h"
#include "LambdaSourceModule.h"
#include "LambdaSourceSettings.h"
#include "SourceGeometryBuilder.h"
#include "SourceCoordinates.h"

USourceStudioModelComponent::USourceStudioModelComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;

	// A view model is drawn, never collided with; collision would also make every re-skin re-cook the mesh.
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetCastShadow(false);
	bUseComplexAsSimpleCollision = false;
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

	SourceGeometry::ApplyToComponent(this, Model->Sections, MaterialLibrary);

	// Show the bind pose until something asks for a sequence, so a model with no animation still renders.
	Model->EvaluateBindPose(BoneToModel);
	CurrentSequence = INDEX_NONE;
	Cycle = 0.0f;
	bSequenceFinished = false;
	bSequenceLooping = false;
	Gestures.Reset();
	return true;
}

void USourceStudioModelComponent::ClearModel()
{
	ClearAllMeshSections();
	Model.Reset();
	ModelPath.Reset();
	BoneToModel.Reset();
	CurrentSequence = INDEX_NONE;
	Cycle = 0.0f;
	bSequenceFinished = false;
	bSequenceLooping = false;
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

bool USourceStudioModelComponent::PlaySequence(int32 SequenceIndex)
{
	if (!HasModel() || !Model->GetSequences().IsValidIndex(SequenceIndex))
	{
		return false;
	}
	CurrentSequence = SequenceIndex;
	Cycle = 0.0f;
	bSequenceLooping = Model->IsSequenceLooping(SequenceIndex);
	bSequenceFinished = false;

	ComposePose();
	return true;
}

void USourceStudioModelComponent::ComposePose()
{
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
	for (const FGestureLayer& Gesture : Gestures)
	{
		Model->AccumulateSequence(Pose, Gesture.Sequence, Gesture.Cycle, 1.0f);
	}
	Model->BuildBoneToModel(Pose, BoneToModel);
	RefreshPose();
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
	ClearAllMeshSections();
	SourceGeometry::ApplyToComponent(this, Model->Sections, MaterialLibrary);
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

	if (!HasModel() || !IsVisible() || bExternalPose)
	{
		return;
	}

	// Gesture layers run on their own cycles and drop off when done (CBaseAnimatingOverlay::StudioFrameAdvance).
	bool bGesturesChanged = false;
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

void USourceStudioModelComponent::RefreshPose()
{
	if (!HasModel())
	{
		return;
	}
	Model->ApplyPose(BoneToModel);

	// Only positions and normals change; the index buffer, UVs and colours are untouched.
	for (int32 i = 0; i < Model->Sections.Num(); ++i)
	{
		const FSourceMeshSection& Section = Model->Sections[i];
		UpdateMeshSection_LinearColor(i, Section.Vertices, Section.Normals, Section.UV0, Section.Colors, Section.Tangents);
	}
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
