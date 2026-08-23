#include "SourceParticleEffect.h"
#include "LambdaMaterialLibrary.h"
#include "LambdaSourceModule.h"
#include "ProceduralMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInterface.h"

ASourceParticleEffect::ASourceParticleEffect()
{
	PrimaryActorTick.bCanEverTick = true;
	Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Sprites"));
	SetRootComponent(Mesh);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->bUseComplexAsSimpleCollision = false;
}

ASourceParticleEffect* ASourceParticleEffect::Create(UWorld* World, const FVector& Origin, ULambdaMaterialLibrary* Materials)
{
	if (!World)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ASourceParticleEffect* Effect = World->SpawnActor<ASourceParticleEffect>(ASourceParticleEffect::StaticClass(), FTransform(Origin), Params);
	if (Effect)
	{
		Effect->MaterialLibrary = Materials;
	}
	return Effect;
}

int32 ASourceParticleEffect::AddMaterial(const FString& SourceMaterialName)
{
	UMaterialInterface* Material = MaterialLibrary ? MaterialLibrary->GetSpriteMaterial(SourceMaterialName) : nullptr;
	return Materials.Add(Material);
}

void ASourceParticleEffect::AddParticle(const FSourceSimpleParticle& Particle)
{
	Particles.Add(Particle);
	bEverHadParticles = true;
}

void ASourceParticleEffect::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// CSimpleEmitter::SimulateParticles: gravity, motion, age; a particle past its die time is gone.
	for (int32 i = Particles.Num() - 1; i >= 0; --i)
	{
		FSourceSimpleParticle& P = Particles[i];
		P.Lifetime += DeltaSeconds;
		if (P.Lifetime >= P.DieTime)
		{
			Particles.RemoveAtSwap(i);
			continue;
		}
		P.Velocity.Z -= Gravity * DeltaSeconds;
		P.Position += P.Velocity * DeltaSeconds;
		P.Roll += P.RollDelta * DeltaSeconds;
	}

	if (Particles.Num() == 0)
	{
		if (bEverHadParticles)
		{
			Destroy();
		}
		return;
	}
	RebuildMesh();
}

void ASourceParticleEffect::RebuildMesh()
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC || !Mesh)
	{
		return;
	}
	FVector CamLoc;
	FRotator CamRot;
	PC->GetPlayerViewPoint(CamLoc, CamRot);
	const FVector CamRight = FRotationMatrix(CamRot).GetUnitAxis(EAxis::Y);
	const FVector CamUp = FRotationMatrix(CamRot).GetUnitAxis(EAxis::Z);
	const FVector CamFwd = CamRot.Vector();

	// One section per material; particles are plain camera-facing quads with their colour ramp in the vertex colour.
	const FVector Origin = GetActorLocation();
	for (int32 m = 0; m < Materials.Num(); ++m)
	{
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TArray<FVector2D> UV1;	// x = alpha; the sprite masters read the fade from here (see create_assets.py)
		TArray<FLinearColor> Colors;
		TArray<FProcMeshTangent> Tangents;

		for (const FSourceSimpleParticle& P : Particles)
		{
			if (P.MaterialIndex != m)
			{
				continue;
			}
			const float T = FMath::Clamp(P.Lifetime / FMath::Max(P.DieTime, KINDA_SMALL_NUMBER), 0.0f, 1.0f);
			const float Size = FMath::Lerp(P.StartSize, P.EndSize, T);
			const float Alpha = FMath::Lerp(P.StartAlpha, P.EndAlpha, T);
			const float Half = Size * 0.5f;
			const FVector Right = (CamRight * FMath::Cos(P.Roll) + CamUp * FMath::Sin(P.Roll)) * Half;
			const FVector Up = (CamUp * FMath::Cos(P.Roll) - CamRight * FMath::Sin(P.Roll)) * Half;
			const FVector Centre = P.Position - Origin;	// component space

			const int32 Base = Vertices.Num();
			Vertices.Add(Centre - Right - Up);
			Vertices.Add(Centre + Right - Up);
			Vertices.Add(Centre + Right + Up);
			Vertices.Add(Centre - Right + Up);
			UVs.Add(FVector2D(0, 1)); UVs.Add(FVector2D(1, 1)); UVs.Add(FVector2D(1, 0)); UVs.Add(FVector2D(0, 0));
			const FLinearColor Color(P.Color.R, P.Color.G, P.Color.B, Alpha);
			for (int32 v = 0; v < 4; ++v)
			{
				Normals.Add(-CamFwd);
				Colors.Add(Color);
				UV1.Add(FVector2D(Alpha, 0.0));
				Tangents.Add(FProcMeshTangent(CamRight, false));
			}
			Triangles.Append({ Base, Base + 2, Base + 1, Base, Base + 3, Base + 2 });
		}

		if (Vertices.Num() == 0)
		{
			Mesh->ClearMeshSection(m);
			continue;
		}
		const TArray<FVector2D> NoUV;
		Mesh->CreateMeshSection_LinearColor(m, Vertices, Triangles, Normals, UVs, UV1, NoUV, NoUV, Colors, Tangents, /*bCreateCollision=*/ false);
		Mesh->SetMaterial(m, Materials[m]);
	}
}
