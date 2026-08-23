#include "SourceStudioModelComponent.h"
#include "LambdaMaterialLibrary.h"
#include "LambdaSourceModule.h"
#include "LambdaSourceSettings.h"
#include "SourceGeometryBuilder.h"

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

	Model->EvaluateSequence(CurrentSequence, Cycle, BoneToModel);
	RefreshPose();
	return true;
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

	if (!HasModel() || CurrentSequence == INDEX_NONE || !IsVisible())
	{
		return;
	}

	const float Duration = Model->GetSequenceDuration(CurrentSequence);
	if (Duration <= 0.0f)
	{
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

	if (!FMath::IsNearlyEqual(PrevCycle, Cycle))
	{
		Model->EvaluateSequence(CurrentSequence, Cycle, BoneToModel);
		RefreshPose();
	}
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
