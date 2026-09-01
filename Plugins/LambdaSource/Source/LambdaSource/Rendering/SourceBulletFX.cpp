#include "Rendering/SourceBulletFX.h"

#include "Core/LambdaSourceSettings.h"
#include "Materials/LambdaMaterialLibrary.h"
#include "ProceduralMeshComponent.h"
#include "Components/PointLightComponent.h"

namespace
{
	/** How long each thing is on screen. Both are a couple of frames: a tracer you can study is a laser. */
	constexpr float TracerSeconds = 0.06f;
	constexpr float FlashSeconds = 0.05f;

	ASourceBulletFX* SpawnFX(UWorld* World, const FVector& Where)
	{
		if (!World)
		{
			return nullptr;
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.ObjectFlags |= RF_Transient;
		return World->SpawnActor<ASourceBulletFX>(ASourceBulletFX::StaticClass(), Where, FRotator::ZeroRotator, Params);
	}
}

ASourceBulletFX::ASourceBulletFX()
{
	PrimaryActorTick.bCanEverTick = true;

	Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("FX"));
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
	Mesh->SetMobility(EComponentMobility::Movable);
	SetRootComponent(Mesh);

	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("FlashLight"));
	Light->SetupAttachment(Mesh);
	Light->SetMobility(EComponentMobility::Movable);
	Light->SetCastShadows(false);
	Light->SetVisibility(false);
	Light->SetIntensityUnits(ELightUnits::Candelas);
	Light->SetLightColor(FLinearColor(FColor(255, 192, 64)));
}

void ASourceBulletFX::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (GetWorld() && GetWorld()->GetTimeSeconds() >= DieTime)
	{
		Destroy();
	}
}

void ASourceBulletFX::Tracer(UWorld* World, const FVector& Start, const FVector& End, ULambdaMaterialLibrary* Materials)
{
	// Every fourth round, as Source does (iTracerFreq): the gaps are what let the eye read the direction.
	if (FMath::RandRange(0, 3) != 0)
	{
		return;
	}
	if (ASourceBulletFX* FX = SpawnFX(World, Start))
	{
		FX->DieTime = World->GetTimeSeconds() + TracerSeconds;
		FX->BuildTracer(Start, End, Materials);
	}
}

void ASourceBulletFX::MuzzleFlash(UWorld* World, const FVector& Position, const FVector& Direction,
	ULambdaMaterialLibrary* Materials)
{
	if (ASourceBulletFX* FX = SpawnFX(World, Position))
	{
		FX->DieTime = World->GetTimeSeconds() + FlashSeconds;
		FX->BuildFlash(Position, Direction, Materials);
	}
}

void ASourceBulletFX::BuildTracer(const FVector& Start, const FVector& End, ULambdaMaterialLibrary* Materials)
{
	const FVector Along = End - Start;
	const float Length = Along.Size();
	if (Length < 1.0f)
	{
		return;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const FVector Dir = Along / Length;

	// A flat strip along the shot, turned to face the camera. Source builds its tracer the same way and
	// leaves the last stretch of it visible rather than the whole flight, so a long shot does not become a
	// solid beam from muzzle to wall.
	const float DrawLength = FMath::Min(Length, 300.0f * Scale);
	const FVector TailStart = End - Dir * DrawLength;
	const float HalfWidth = 1.2f * Scale;

	// Facing is worked out once, from the strip to the camera - close enough for six hundredths of a second.
	FVector ToEye = FVector::UpVector;
	if (const UWorld* World = GetWorld())
	{
		if (const APlayerController* PC = World->GetFirstPlayerController())
		{
			FVector EyeLoc; FRotator EyeRot;
			PC->GetPlayerViewPoint(EyeLoc, EyeRot);
			ToEye = (EyeLoc - (TailStart + Dir * (DrawLength * 0.5f))).GetSafeNormal();
		}
	}
	FVector Side = FVector::CrossProduct(Dir, ToEye).GetSafeNormal();
	if (Side.IsNearlyZero())
	{
		Side = FVector::CrossProduct(Dir, FVector::UpVector).GetSafeNormal();
	}

	const FTransform ToLocal = GetActorTransform().Inverse();
	const FVector A = ToLocal.TransformPosition(TailStart + Side * HalfWidth);
	const FVector B = ToLocal.TransformPosition(TailStart - Side * HalfWidth);
	const FVector C = ToLocal.TransformPosition(End - Side * HalfWidth);
	const FVector D = ToLocal.TransformPosition(End + Side * HalfWidth);

	TArray<FVector> Verts = { A, B, C, D };
	TArray<int32> Tris = { 0, 1, 2, 0, 2, 3 };
	TArray<FVector> Normals = { -Dir, -Dir, -Dir, -Dir };
	TArray<FVector2D> UVs = { FVector2D(0, 0), FVector2D(0, 1), FVector2D(1, 1), FVector2D(1, 0) };
	TArray<FLinearColor> Colors = { FLinearColor::White, FLinearColor::White, FLinearColor::White, FLinearColor::White };
	TArray<FProcMeshTangent> Tangents;
	Mesh->CreateMeshSection_LinearColor(0, Verts, Tris, Normals, UVs, Colors, Tangents, /*bCreateCollision=*/ false);
	if (Materials)
	{
		if (UMaterialInterface* Material = Materials->GetSpriteMaterial(TEXT("effects/tracer_middle")))
		{
			Mesh->SetMaterial(0, Material);
		}
	}
}

void ASourceBulletFX::BuildFlash(const FVector& Position, const FVector& Direction, ULambdaMaterialLibrary* Materials)
{
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const FVector Dir = Direction.GetSafeNormal();

	// One camera-facing quad at the muzzle. Source layers several scaled sprites; one reads the same at the
	// distance anyone ever sees an NPC's muzzle from, and costs a quad instead of five.
	FVector ToEye = -Dir;
	if (const UWorld* World = GetWorld())
	{
		if (const APlayerController* PC = World->GetFirstPlayerController())
		{
			FVector EyeLoc; FRotator EyeRot;
			PC->GetPlayerViewPoint(EyeLoc, EyeRot);
			ToEye = (EyeLoc - Position).GetSafeNormal();
		}
	}
	FVector Right = FVector::CrossProduct(ToEye, FVector::UpVector).GetSafeNormal();
	if (Right.IsNearlyZero())
	{
		Right = FVector::RightVector;
	}
	const FVector Up = FVector::CrossProduct(Right, ToEye).GetSafeNormal();
	const float Size = FMath::FRandRange(7.0f, 10.0f) * Scale;

	const FTransform ToLocal = GetActorTransform().Inverse();
	const FVector Centre = Position + Dir * (4.0f * Scale);
	TArray<FVector> Verts = {
		ToLocal.TransformPosition(Centre - Right * Size + Up * Size),
		ToLocal.TransformPosition(Centre - Right * Size - Up * Size),
		ToLocal.TransformPosition(Centre + Right * Size - Up * Size),
		ToLocal.TransformPosition(Centre + Right * Size + Up * Size) };
	TArray<int32> Tris = { 0, 1, 2, 0, 2, 3 };
	TArray<FVector> Normals = { ToEye, ToEye, ToEye, ToEye };
	TArray<FVector2D> UVs = { FVector2D(0, 0), FVector2D(0, 1), FVector2D(1, 1), FVector2D(1, 0) };
	TArray<FLinearColor> Colors = { FLinearColor::White, FLinearColor::White, FLinearColor::White, FLinearColor::White };
	TArray<FProcMeshTangent> Tangents;
	Mesh->CreateMeshSection_LinearColor(0, Verts, Tris, Normals, UVs, Colors, Tangents, /*bCreateCollision=*/ false);
	if (Materials)
	{
		const int32 Which = FMath::RandRange(1, 4);
		if (UMaterialInterface* Material = Materials->GetSpriteMaterial(FString::Printf(TEXT("effects/muzzleflash%d"), Which)))
		{
			Mesh->SetMaterial(0, Material);
		}
	}

	// The light Source's ProcessMuzzleFlashEvent allocates, at the settings' own brightness so the player's
	// flash and an NPC's are lit by the same number.
	const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();
	if (Settings.bMuzzleFlashLight && Light)
	{
		Light->SetWorldLocation(Position);
		Light->SetAttenuationRadius(Settings.MuzzleFlashLightRadiusUnits * Scale);
		Light->SetIntensity(Settings.MuzzleFlashLightIntensity);
		Light->SetLightingChannels(true, true, false);
		Light->SetVisibility(true);
	}
}
