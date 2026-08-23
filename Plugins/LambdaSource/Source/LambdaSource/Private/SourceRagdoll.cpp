#include "SourceRagdoll.h"
#include "LambdaSourceModule.h"
#include "SourceCoordinates.h"
#include "SourceNPCBase.h"
#include "SourcePHYFile.h"
#include "SourceStudioModelComponent.h"
#include "Engine/World.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "ProceduralMeshComponent.h"

namespace
{
	/**
	 * A Source bone matrix (column vectors, Source axes, units) as a UE transform in model space: the mirror
	 * y -> -y applied on both sides, so X = forward, Y = -left, Z = up.
	 */
	FTransform SourceBoneToUE(const FSourceMatrix3x4& M, float Scale)
	{
		const FVector X = FSourceCoords::ToUEDirection(M.GetForward());
		const FVector Y = -FSourceCoords::ToUEDirection(M.GetLeft());
		const FVector Z = FSourceCoords::ToUEDirection(M.GetUp());
		const FVector Origin = FSourceCoords::ToUE(M.GetOrigin(), Scale);
		return FTransform(FMatrix(X, Y, Z, Origin));
	}

	FSourceMatrix3x4 UEToSourceBone(const FTransform& T, float Scale)
	{
		const FMatrix M = T.ToMatrixNoScale();
		const FVector X = M.GetUnitAxis(EAxis::X);
		const FVector Y = M.GetUnitAxis(EAxis::Y);
		const FVector Z = M.GetUnitAxis(EAxis::Z);
		const FVector3f F((float)X.X, (float)-X.Y, (float)X.Z);
		const FVector3f L((float)-Y.X, (float)Y.Y, (float)-Y.Z);
		const FVector3f U((float)Z.X, (float)-Z.Y, (float)Z.Z);
		const FVector3f O = FSourceCoords::ToSource(T.GetLocation(), Scale);
		FSourceMatrix3x4 Out;
		for (int32 r = 0; r < 3; ++r)
		{
			Out.M[r][0] = F[r];
			Out.M[r][1] = L[r];
			Out.M[r][2] = U[r];
			Out.M[r][3] = O[r];
		}
		return Out;
	}

	// Joint friction is not a thing UE's constraint has; Source's per-axis "xfriction" is a torque that barely
	// registers on a headcrab, and the bodies carry their own rotdamping.
	void SetAngularLimit(UPhysicsConstraintComponent* Joint, int32 Axis, float Min, float Max)
	{
		const float Half = FMath::Max(0.0f, (Max - Min) * 0.5f);
		const EAngularConstraintMotion Motion = Half < 0.5f ? ACM_Locked : ACM_Limited;
		switch (Axis)
		{
		case 0: Joint->SetAngularTwistLimit(Motion, Half); break;		// x: twist, about the joint's X
		case 1: Joint->SetAngularSwing2Limit(Motion, Half); break;		// y: swing 2, about Y
		default: Joint->SetAngularSwing1Limit(Motion, Half); break;	// z: swing 1, about Z
		}
	}
}

ASourceRagdoll::ASourceRagdoll()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;	// read the bodies after the simulation step
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
	BloodColor = ESourceBloodColor::Red;
}

ASourceRagdoll* ASourceRagdoll::Create(UWorld* World, USourceStudioModelComponent* ModelComponent, const FSourcePHYFile& Phy,
	const FVector& ForceImpulse, const FVector& ForcePosition, const FVector& InitialVelocity,
	ESourceBloodColor InBloodColor, float Lifetime)
{
	if (!World || !ModelComponent || !ModelComponent->HasModel() || Phy.GetSolids().Num() == 0)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ASourceRagdoll* Ragdoll = World->SpawnActor<ASourceRagdoll>(ASourceRagdoll::StaticClass(), ModelComponent->GetComponentTransform(), Params);
	if (!Ragdoll)
	{
		return nullptr;
	}
	Ragdoll->BloodColor = InBloodColor;
	if (!Ragdoll->Build(ModelComponent, Phy))
	{
		Ragdoll->Destroy();
		return nullptr;
	}
	Ragdoll->ApplyForces(ForceImpulse, ForcePosition, InitialVelocity);
	if (Lifetime > 0.0f)
	{
		Ragdoll->SetLifeSpan(Lifetime);
	}
	return Ragdoll;
}

bool ASourceRagdoll::Build(USourceStudioModelComponent* ModelComponent, const FSourcePHYFile& Phy)
{
	const FSourceMDLFile* Mdl = ModelComponent->GetModel();
	const TArray<FSourceStudioBone>& Bones = Mdl->GetBones();
	const TArray<FSourceMatrix3x4>& Pose = ModelComponent->GetBoneToModel();
	if (Pose.Num() != Bones.Num())
	{
		return false;
	}
	const float Scale = FSourceCoords::GetUnitScale();

	Model = ModelComponent;
	ModelToWorld = ModelComponent->GetComponentTransform();
	DeathPose.SetNum(Bones.Num());
	BoneBody.Init(INDEX_NONE, Bones.Num());
	for (int32 b = 0; b < Bones.Num(); ++b)
	{
		DeathPose[b] = SourceBoneToUE(Pose[b], Scale);
	}

	// RagdollAddSolid: one physics object per solid, placed on its bone's current matrix.
	TArray<int32> SolidBody;
	SolidBody.Init(INDEX_NONE, Phy.GetSolids().Num());
	for (const FSourcePHYSolid& Solid : Phy.GetSolids())
	{
		int32 BoneIndex = INDEX_NONE;
		for (int32 b = 0; b < Bones.Num(); ++b)
		{
			if (Bones[b].Name.Equals(Solid.BoneName, ESearchCase::IgnoreCase))
			{
				BoneIndex = b;
				break;
			}
		}
		if (BoneIndex == INDEX_NONE || Solid.Hulls.Num() == 0)
		{
			UE_LOG(LogLambdaSource, Warning, TEXT("Ragdoll: couldn't lookup bone '%s' for solid %d"), *Solid.BoneName, Solid.Index);
			continue;
		}

		UProceduralMeshComponent* Body = NewObject<UProceduralMeshComponent>(this, *FString::Printf(TEXT("Solid_%d"), Solid.Index));
		Body->bUseComplexAsSimpleCollision = false;
		Body->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Body->SetCollisionObjectType(ECC_PhysicsBody);
		Body->SetCollisionResponseToAllChannels(ECR_Block);
		// Ragdoll parts do not collide with each other unless the model asks (collisionrules selfcollisions),
		// and like Source's debris they never shove the player around.
		if (!Phy.AllowsSelfCollisions())
		{
			Body->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Ignore);
		}
		Body->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		Body->SetCastShadow(false);
		Body->SetGenerateOverlapEvents(false);
		Body->RegisterComponent();
		for (const TArray<FVector>& Hull : Solid.Hulls)
		{
			Body->AddCollisionConvexMesh(Hull);
		}
		Body->SetWorldTransform(DeathPose[BoneIndex] * ModelToWorld);
		Body->SetSimulatePhysics(true);
		Body->SetMassOverrideInKg(NAME_None, FMath::Max(0.1f, Solid.Mass), true);
		Body->SetLinearDamping(Solid.Damping);
		Body->SetAngularDamping(Solid.RotDamping);

		SolidBody[Solid.Index] = Bodies.Num();
		Bodies.Add(Body);
		BodyBone.Add(BoneIndex);
		BoneBody[BoneIndex] = Bodies.Num() - 1;
		TotalMass += Solid.Mass;
	}
	if (Bodies.Num() == 0)
	{
		return false;
	}

	// RagdollAddConstraint: the joint sits at the child's origin with the parent's axes (constraintToReference is
	// the identity, constraintToAttached the child-to-parent transform); limits are degrees about those axes.
	// UE's limits are symmetric, so each axis gets its half-range and the parent frame is turned to the centre.
	for (const FSourcePHYConstraint& C : Phy.GetConstraints())
	{
		if (!SolidBody.IsValidIndex(C.ParentSolid) || !SolidBody.IsValidIndex(C.ChildSolid)
			|| SolidBody[C.ParentSolid] == INDEX_NONE || SolidBody[C.ChildSolid] == INDEX_NONE || C.ParentSolid == C.ChildSolid)
		{
			continue;
		}
		UProceduralMeshComponent* Parent = Bodies[SolidBody[C.ParentSolid]];
		UProceduralMeshComponent* Child = Bodies[SolidBody[C.ChildSolid]];

		UPhysicsConstraintComponent* Joint = NewObject<UPhysicsConstraintComponent>(this, *FString::Printf(TEXT("Joint_%d_%d"), C.ParentSolid, C.ChildSolid));
		Joint->RegisterComponent();
		const FVector Centre = (C.Min + C.Max) * 0.5f;
		const FQuat ParentRot = Parent->GetComponentQuat();
		const FQuat Offset = FQuat(FVector::XAxisVector, FMath::DegreesToRadians(Centre.X))
			* FQuat(FVector::YAxisVector, FMath::DegreesToRadians(Centre.Y))
			* FQuat(FVector::ZAxisVector, FMath::DegreesToRadians(Centre.Z));
		Joint->SetWorldTransform(FTransform(ParentRot * Offset, Child->GetComponentLocation()));
		Joint->SetConstrainedComponents(Child, NAME_None, Parent, NAME_None);
		Joint->SetDisableCollision(true);
		Joint->SetLinearXLimit(LCM_Locked, 0.0f);
		Joint->SetLinearYLimit(LCM_Locked, 0.0f);
		Joint->SetLinearZLimit(LCM_Locked, 0.0f);
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			SetAngularLimit(Joint, Axis, C.Min[Axis], C.Max[Axis]);
		}
		Joints.Add(Joint);
	}

	ModelComponent->SetExternalPose(Pose);
	UE_LOG(LogLambdaSource, Log, TEXT("Ragdoll: %d bodies, %d joints, %.2f kg"), Bodies.Num(), Joints.Num(), TotalMass);
	return true;
}

void ASourceRagdoll::ApplyForces(const FVector& ForceImpulse, const FVector& ForcePosition, const FVector& InitialVelocity)
{
	// C_ClientRagdoll starts from the animation's bone velocities; the character's own velocity is the part of
	// that we have.
	for (UProceduralMeshComponent* Body : Bodies)
	{
		Body->SetPhysicsLinearVelocity(InitialVelocity);
	}
	if (ForceImpulse.IsNearlyZero())
	{
		return;
	}

	// RagdollCreate: the whole force on the bone that was hit, and a mass-weighted share on every other body
	// from that point.
	int32 ForceBody = 0;
	float Best = TNumericLimits<float>::Max();
	for (int32 i = 0; i < Bodies.Num(); ++i)
	{
		const float D = FVector::DistSquared(Bodies[i]->GetComponentLocation(), ForcePosition);
		if (D < Best)
		{
			Best = D;
			ForceBody = i;
		}
	}
	Bodies[ForceBody]->AddImpulse(ForceImpulse);
	const FVector Origin = Bodies[ForceBody]->GetComponentLocation();
	const float Total = FMath::Max(TotalMass, 1.0f);
	for (int32 i = 0; i < Bodies.Num(); ++i)
	{
		if (i != ForceBody)
		{
			const float Share = Bodies[i]->GetMass() / Total;
			Bodies[i]->AddImpulseAtLocation(ForceImpulse * Share, Origin);
		}
	}
}

void ASourceRagdoll::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdatePose();
}

void ASourceRagdoll::UpdatePose()
{
	USourceStudioModelComponent* ModelComponent = Model.Get();
	if (!ModelComponent || !ModelComponent->HasModel())
	{
		return;
	}
	const TArray<FSourceStudioBone>& Bones = ModelComponent->GetModel()->GetBones();
	if (Bones.Num() != DeathPose.Num())
	{
		return;
	}
	const float Scale = FSourceCoords::GetUnitScale();

	// Simulated bones come from their bodies; the rest keep their death-pose offset from the nearest simulated
	// ancestor (C_ClientRagdoll: ACT_DIERAGDOLL supplies the non-simulated bones).
	TArray<FTransform> BoneModel;
	BoneModel.SetNum(Bones.Num());
	TArray<FSourceMatrix3x4> Pose;
	Pose.SetNum(Bones.Num());
	for (int32 b = 0; b < Bones.Num(); ++b)
	{
		if (BoneBody[b] != INDEX_NONE)
		{
			BoneModel[b] = Bodies[BoneBody[b]]->GetComponentTransform().GetRelativeTransform(ModelToWorld);
		}
		else if (Bones[b].Parent >= 0 && Bones[b].Parent < b)
		{
			const FTransform Relative = DeathPose[b].GetRelativeTransform(DeathPose[Bones[b].Parent]);
			BoneModel[b] = Relative * BoneModel[Bones[b].Parent];
		}
		else
		{
			BoneModel[b] = DeathPose[b];
		}
		Pose[b] = UEToSourceBone(BoneModel[b], Scale);
	}
	ModelComponent->SetExternalPose(Pose);
}
