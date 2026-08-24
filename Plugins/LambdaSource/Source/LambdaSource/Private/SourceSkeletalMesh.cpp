#include "SourceSkeletalMesh.h"

#include "LambdaMaterialLibrary.h"
#include "LambdaSourceModule.h"
#include "LambdaSourceSettings.h"
#include "SourceCoordinates.h"
#include "SourceGeometryBuilder.h"
#include "SourceMDLFile.h"

#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "ReferenceSkeleton.h"
#include "Components/PoseableMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"
#include "UObject/UObjectIterator.h"

namespace
{
	/** Every mesh built so far, by model path and bodygroup selection. Rooted: nothing else holds them. */
	TMap<FString, TObjectPtr<USkeletalMesh>>& GetMeshCache()
	{
		static TMap<FString, TObjectPtr<USkeletalMesh>> Cache;
		return Cache;
	}
}

void FSourceSkeletalMesh::FlushCache()
{
	for (TPair<FString, TObjectPtr<USkeletalMesh>>& Pair : GetMeshCache())
	{
		if (Pair.Value)
		{
			Pair.Value->RemoveFromRoot();
		}
	}
	GetMeshCache().Reset();
}

USkeletalMesh* FSourceSkeletalMesh::GetOrBuild(const FString& ModelPath, const FSourceMDLFile& Model,
	ULambdaMaterialLibrary* Materials, const FString& BodygroupKey)
{
	const FString Key = ModelPath.ToLower() + TEXT("|") + BodygroupKey;
	if (TObjectPtr<USkeletalMesh>* Found = GetMeshCache().Find(Key))
	{
		if (*Found)
		{
			return *Found;
		}
	}
	USkeletalMesh* Mesh = Build(Model, Materials, ModelPath);
	if (Mesh)
	{
		Mesh->AddToRoot();
		GetMeshCache().Add(Key, Mesh);
	}
	return Mesh;
}

USkeletalMesh* FSourceSkeletalMesh::Build(const FSourceMDLFile& Model, ULambdaMaterialLibrary* Materials, const FString& DebugName)
{
	const TArray<FSourceStudioBone>& Bones = Model.GetBones();
	const TArray<TArray<FSourceSkinVertex>>& SkinVertices = Model.GetSkinVertices();
	if (Bones.Num() == 0 || SkinVertices.Num() != Model.Sections.Num() || Model.Sections.Num() == 0)
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Skeletal mesh '%s': nothing to build from"), *DebugName);
		return nullptr;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;

	USkeletalMesh* Mesh = NewObject<USkeletalMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	USkeleton* Skeleton = NewObject<USkeleton>(GetTransientPackage(), NAME_None, RF_Transient);

	// ---- reference skeleton ------------------------------------------------------------------------------
	// The bind pose is taken through the same conversion the rest of the engine uses, as model-space transforms,
	// and turned into the parent-relative ones a reference skeleton wants. Going via model space avoids having
	// to mirror Source's local rotations by hand.
	TArray<FSourceMatrix3x4> BindPose;
	Model.EvaluateBindPose(BindPose);
	TArray<FTransform> BindComponentSpace;
	BindComponentSpace.Reserve(Bones.Num());
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		BindComponentSpace.Add(BindPose.IsValidIndex(i) ? BindPose[i].ToUETransform(Scale) : FTransform::Identity);
	}

	FReferenceSkeleton RefSkeleton;
	{
		FReferenceSkeletonModifier Modifier(RefSkeleton, nullptr);
		for (int32 i = 0; i < Bones.Num(); ++i)
		{
			// A reference skeleton requires every parent to come before its child; studiomdl writes bones in
			// that order already.
			//
			// It also has to have exactly one root, whatever FReferenceSkeletonModifier's bAllowMultipleRoots
			// suggests: USkeletalMesh::CalculateInvRefMatrices composes bone b>0 against its parent without ever
			// checking for INDEX_NONE, so a second parentless bone indexes off the front of the array. Source
			// models do have them - v_crowbar.mdl is one - so a stray root is hung off bone 0 instead. Its bind
			// pose is unchanged: the local transform is worked out against whatever it ends up parented to.
			int32 Parent = (Bones[i].Parent >= 0 && Bones[i].Parent < i) ? Bones[i].Parent : INDEX_NONE;
			if (i > 0 && Parent == INDEX_NONE)
			{
				Parent = 0;
			}
			FTransform Local = BindComponentSpace[i];
			if (Parent != INDEX_NONE)
			{
				Local = BindComponentSpace[i] * BindComponentSpace[Parent].Inverse();
			}
			Local.NormalizeRotation();
			const FName BoneName(*Bones[i].Name);
			Modifier.Add(FMeshBoneInfo(BoneName, Bones[i].Name, Parent), Local);
		}
	}	// the modifier's destructor is what rebuilds the skeleton's lookup tables

	UE_LOG(LogLambdaSource, Verbose, TEXT("  skel '%s': %d source bones -> %d reference bones"),
		*DebugName, Bones.Num(), RefSkeleton.GetRawBoneNum());
	Mesh->SetRefSkeleton(RefSkeleton);
	Mesh->CalculateInvRefMatrices();
	UE_LOG(LogLambdaSource, Verbose, TEXT("  skel '%s': inv ref matrices done"), *DebugName);
	Skeleton->MergeAllBonesToBoneTree(Mesh);
	Mesh->SetSkeleton(Skeleton);
	UE_LOG(LogLambdaSource, Verbose, TEXT("  skel '%s': bone tree merged"), *DebugName);

	Mesh->SetHasVertexColors(false);
	Mesh->SetEnablePerPolyCollision(false);
	Mesh->NeverStream = true;

	// ---- vertices, weights, indices ----------------------------------------------------------------------
	Mesh->AllocateResourceForRendering();
	FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
	if (!RenderData)
	{
		return nullptr;
	}
	FSkeletalMeshLODRenderData* LOD = new FSkeletalMeshLODRenderData();
	RenderData->LODRenderData.Add(LOD);

	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector3f> TangentsX;
	TArray<FVector2f> UVs;
	TArray<FSkinWeightInfo> SkinWeights;
	TArray<uint32> Indices;
	FBox Bounds(ForceInit);

	for (int32 s = 0; s < Model.Sections.Num(); ++s)
	{
		const FSourceMeshSection& Section = Model.Sections[s];
		const TArray<FSourceSkinVertex>& Skin = SkinVertices[s];
		const int32 NumVerts = FMath::Min(Section.Vertices.Num(), Skin.Num());
		if (NumVerts == 0 || Section.Triangles.Num() == 0)
		{
			continue;
		}

		FSkelMeshRenderSection& Out = LOD->RenderSections.AddDefaulted_GetRef();
		Out.BaseVertexIndex = Positions.Num();
		Out.BaseIndex = Indices.Num();
		Out.NumTriangles = Section.Triangles.Num() / 3;
		Out.NumVertices = NumVerts;
		Out.MaxBoneInfluences = 1;
		Out.MaterialIndex = Mesh->GetMaterials().Num();
		Out.bDisabled = false;

		// The material a section draws with. Its name is the one the model asked for; the library has already
		// resolved it to a VMT.
		UMaterialInterface* Material = Materials ? Materials->GetMaterial(Section.MaterialName) : nullptr;
		FSkeletalMaterial SkeletalMaterial(Material, FName(*Section.MaterialName));
		// Cooked builds never fill this in, and the streaming code ensures on it.
		SkeletalMaterial.UVChannelData = FMeshUVChannelInfo(1.0f);
		Mesh->GetMaterials().Add(SkeletalMaterial);

		for (int32 v = 0; v < NumVerts; ++v)
		{
			const FSourceSkinVertex& In = Skin[v];
			const FVector3f Position = (FVector3f)FSourceCoords::ToUE(In.Position, Scale);
			Positions.Add(Position);
			Normals.Add((FVector3f)FSourceCoords::ToUEDirection(In.Normal.GetSafeNormal()));
			TangentsX.Add(Section.Tangents.IsValidIndex(v) ? (FVector3f)Section.Tangents[v].TangentX : FVector3f(1, 0, 0));
			UVs.Add(Section.UV0.IsValidIndex(v) ? FVector2f(Section.UV0[v]) : FVector2f::ZeroVector);
			Bounds += (FVector)Position;

			// A vertex's influences are stored against the section's own bone list, not the skeleton's: the
			// renderer uploads one matrix per entry of BoneMap per section and the shader indexes that.
			FSkinWeightInfo Weight;
			FMemory::Memzero(Weight);
			int32 Written = 0;
			uint32 TotalWeight = 0;
			for (int32 b = 0; b < In.NumBones && Written < MAX_TOTAL_INFLUENCES; ++b)
			{
				if (In.Bones[b] >= Bones.Num() || In.Weights[b] <= 0.0f)
				{
					continue;
				}
				const int32 Local = Out.BoneMap.AddUnique((FBoneIndexType)In.Bones[b]);
				const uint16 Quantised = (uint16)FMath::Clamp(FMath::RoundToInt(In.Weights[b] * 65535.0f), 0, 65535);
				Weight.InfluenceBones[Written] = (FBoneIndexType)Local;
				Weight.InfluenceWeights[Written] = Quantised;
				TotalWeight += Quantised;
				++Written;
			}
			if (Written == 0)
			{
				// An unweighted vertex still has to belong to something, or it collapses to the origin.
				Weight.InfluenceBones[0] = (FBoneIndexType)Out.BoneMap.AddUnique(0);
				Weight.InfluenceWeights[0] = 65535;
				Written = 1;
			}
			else if (TotalWeight != 65535 && Written > 0)
			{
				// The weights are a normalised stream: they have to add up exactly, so the rounding error goes
				// on the largest influence.
				int32 Largest = 0;
				for (int32 b = 1; b < Written; ++b)
				{
					if (Weight.InfluenceWeights[b] > Weight.InfluenceWeights[Largest])
					{
						Largest = b;
					}
				}
				Weight.InfluenceWeights[Largest] = (uint16)FMath::Clamp(
					(int32)Weight.InfluenceWeights[Largest] + (65535 - (int32)TotalWeight), 0, 65535);
			}
			Out.MaxBoneInfluences = FMath::Max(Out.MaxBoneInfluences, Written);
			SkinWeights.Add(Weight);
		}

		// The LOD's index buffer is one run for the whole mesh; a section's triangles index into it absolutely.
		for (int32 Index : Section.Triangles)
		{
			Indices.Add((uint32)(Out.BaseVertexIndex + Index));
		}
	}

	if (Positions.Num() == 0)
	{
		return nullptr;
	}

	// Bones the renderer has to have a matrix for: everything any section draws with, plus their parents.
	TSet<FBoneIndexType> Active;
	int32 MaxBoneMap = 0;
	for (const FSkelMeshRenderSection& Section : LOD->RenderSections)
	{
		MaxBoneMap = FMath::Max(MaxBoneMap, Section.BoneMap.Num());
		for (FBoneIndexType Bone : Section.BoneMap)
		{
			Active.Add(Bone);
		}
	}
	LOD->ActiveBoneIndices = Active.Array();
	// Never hand the reference skeleton a bone it does not have: it walks parents from these and indexes
	// straight into its own arrays.
	const int32 NumRefBones = Mesh->GetRefSkeleton().GetRawBoneNum();
	LOD->ActiveBoneIndices.RemoveAll([NumRefBones](FBoneIndexType Bone) { return (int32)Bone >= NumRefBones; });
	LOD->ActiveBoneIndices.Sort();
	UE_LOG(LogLambdaSource, Verbose, TEXT("  skel '%s': %d active bones of %d, sorting parents"),
		*DebugName, LOD->ActiveBoneIndices.Num(), NumRefBones);
	Mesh->GetRefSkeleton().EnsureParentsExistAndSort(LOD->ActiveBoneIndices);
	UE_LOG(LogLambdaSource, Verbose, TEXT("  skel '%s': parents sorted"), *DebugName);

	// ...and the bones animation is evaluated for, which is all of them.
	LOD->RequiredBones.Reset(Bones.Num());
	for (int32 i = 0; i < Bones.Num(); ++i)
	{
		LOD->RequiredBones.Add((FBoneIndexType)i);
	}
	LOD->RequiredBones.Sort();

	const int32 NumVertices = Positions.Num();
	LOD->StaticVertexBuffers.StaticMeshVertexBuffer.SetUseFullPrecisionUVs(false);
	LOD->StaticVertexBuffers.PositionVertexBuffer.Init(NumVertices, /*bNeedsCPUAccess=*/ false);
	LOD->StaticVertexBuffers.StaticMeshVertexBuffer.Init(NumVertices, /*NumTexCoords=*/ 1, /*bNeedsCPUAccess=*/ false);
	for (int32 v = 0; v < NumVertices; ++v)
	{
		LOD->StaticVertexBuffers.PositionVertexBuffer.VertexPosition(v) = Positions[v];
		const FVector3f Normal = Normals[v];
		FVector3f TangentX = TangentsX[v];
		// Keep the tangent frame square: drop anything of the tangent that runs along the normal.
		TangentX = (TangentX - Normal * FVector3f::DotProduct(Normal, TangentX)).GetSafeNormal();
		if (TangentX.IsNearlyZero())
		{
			TangentX = FVector3f::CrossProduct(Normal, FVector3f(0, 0, 1)).GetSafeNormal();
			if (TangentX.IsNearlyZero())
			{
				TangentX = FVector3f(1, 0, 0);
			}
		}
		const FVector3f TangentY = FVector3f::CrossProduct(Normal, TangentX);
		LOD->StaticVertexBuffers.StaticMeshVertexBuffer.SetVertexTangents(v, TangentX, TangentY, Normal);
		LOD->StaticVertexBuffers.StaticMeshVertexBuffer.SetVertexUV(v, 0, UVs[v]);
	}

	LOD->SkinWeightVertexBuffer.SetMaxBoneInfluences(4);
	LOD->SkinWeightVertexBuffer.SetUse16BitBoneIndex(MaxBoneMap > 256);
	LOD->SkinWeightVertexBuffer.SetNeedsCPUAccess(false);
	LOD->SkinWeightVertexBuffer = SkinWeights;

	const uint8 IndexSize = (Indices.Num() > 0 && NumVertices > MAX_uint16) ? sizeof(uint32) : sizeof(uint16);
	LOD->MultiSizeIndexContainer.RebuildIndexBuffer(IndexSize, Indices);

	UE_LOG(LogLambdaSource, Verbose, TEXT("  skel '%s': buffers filled, adding LOD info"), *DebugName);
	FSkeletalMeshLODInfo& LODInfo = Mesh->AddLODInfo();
	LODInfo.ScreenSize = 1.0f;
	LODInfo.LODHysteresis = 0.02f;

	UE_LOG(LogLambdaSource, Verbose, TEXT("  skel '%s': LOD info added, init resources"), *DebugName);
	Mesh->SetImportedBounds(FBoxSphereBounds(Bounds));
	Mesh->InitResources();
	Mesh->RebuildSocketMap();

	UE_LOG(LogLambdaSource, Log, TEXT("Skeletal mesh '%s': %d bones, %d sections, %d verts, %d triangles"),
		*DebugName, Bones.Num(), LOD->RenderSections.Num(), NumVertices, Indices.Num() / 3);
	return Mesh;
}

// ---------------------------------------------------------------------------------------------------------
// lambda.skeltest <model> - builds a skeletal mesh for a model and stands it in front of the player in its
// bind pose, to check the conversion before anything depends on it.
static void LambdaSkelTestCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 1)
	{
		UE_LOG(LogLambdaSource, Display, TEXT("Usage: lambda.skeltest <models/creatures/npc_headcrab/npc_headcrab.mdl>"));
		return;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		return;
	}
	TSharedPtr<FSourceMDLFile> Model = MakeShared<FSourceMDLFile>();
	FString Error;
	if (!Model->Load(Args[0], ULambdaSourceSettings::Get().UnitScale, &Error))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("skeltest: %s"), *Error);
		return;
	}
	ULambdaMaterialLibrary* Materials = nullptr;
	for (TObjectIterator<ULambdaMaterialLibrary> It; It; ++It)
	{
		if (It->GetWorld() == World)
		{
			Materials = *It;
			break;
		}
	}
	USkeletalMesh* Mesh = FSourceSkeletalMesh::GetOrBuild(Args[0], *Model, Materials, TEXT("test"));
	if (!Mesh)
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("skeltest: could not build a skeletal mesh"));
		return;
	}
	const float Distance = Args.Num() > 1 ? FCString::Atof(*Args[1]) : 200.0f;
	const FVector Location = Pawn->GetActorLocation() + Pawn->GetControlRotation().Vector() * Distance;

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(Location), Params);
	if (!Actor)
	{
		return;
	}
	UPoseableMeshComponent* Comp = NewObject<UPoseableMeshComponent>(Actor);
	Actor->SetRootComponent(Comp);
	Comp->RegisterComponent();
	Comp->SetSkinnedAssetAndUpdate(Mesh, true);
	Comp->SetWorldLocation(Location);
	UE_LOG(LogLambdaSource, Display, TEXT("skeltest: '%s' standing at %s"), *Args[0], *Location.ToString());
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaSkelTestCommand(
	TEXT("lambda.skeltest"),
	TEXT("Build a GPU-skinned mesh for a Source model and stand it in front of the player"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaSkelTestCommand));
