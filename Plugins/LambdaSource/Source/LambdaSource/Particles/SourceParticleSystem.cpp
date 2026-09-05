#include "Particles/SourceParticleSystem.h"
#include "Particles/SourceParticleLibrary.h"
#include "Materials/LambdaMaterialLibrary.h"
#include "Formats/SourceVTFFile.h"
#include "Core/LambdaSourceModule.h"
#include "Core/SourceCoordinates.h"
#include "ProceduralMeshComponent.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/Pawn.h"
#include "Materials/MaterialInterface.h"
#include "CollisionQueryParams.h"

using EAttr = ESourceParticleAttr;

namespace
{
	/** A sequence looked up the way the engine does: by its number, then by its slot. */
	const FSourceSheetSequence* FindSequence(const FSourceSpriteSheet& Sheet, int32 Number)
	{
		for (const FSourceSheetSequence& Sequence : Sheet.Sequences)
		{
			if (Sequence.Number == Number)
			{
				return &Sequence;
			}
		}
		return &Sheet.Sequences[FMath::Clamp(Number, 0, Sheet.Sequences.Num() - 1)];
	}

	FVector SafeNormal(const FVector& V, const FVector& Fallback)
	{
		return V.SizeSquared() > 1e-8 ? V.GetSafeNormal() : Fallback;
	}
}

// ---- Geometry scratch ---------------------------------------------------------------------------------------------

void ASourceParticleSystem::FSectionGeometry::Reset()
{
	Vertices.Reset();
	Triangles.Reset();
	Normals.Reset();
	UV0.Reset();
	UV1.Reset();
	UV2.Reset();
	Colors.Reset();
	Tangents.Reset();
}

void ASourceParticleSystem::FSectionGeometry::AddQuad(const FVector Corners[4], const FVector4f& UV, const FVector4f& UV2Rect,
	const FLinearColor Colors4[4], float Alpha4[4], const FVector& Normal, const FVector& Tangent)
{
	// Corners run bottom-left, bottom-right, top-right, top-left; the sheet rectangle is (left, top, right, bottom).
	const int32 Base = Vertices.Num();
	Vertices.Add(Corners[0]);
	Vertices.Add(Corners[1]);
	Vertices.Add(Corners[2]);
	Vertices.Add(Corners[3]);
	UV0.Add(FVector2D(UV.X, UV.W));
	UV0.Add(FVector2D(UV.Z, UV.W));
	UV0.Add(FVector2D(UV.Z, UV.Y));
	UV0.Add(FVector2D(UV.X, UV.Y));
	UV2.Add(FVector2D(UV2Rect.X, UV2Rect.W));
	UV2.Add(FVector2D(UV2Rect.Z, UV2Rect.W));
	UV2.Add(FVector2D(UV2Rect.Z, UV2Rect.Y));
	UV2.Add(FVector2D(UV2Rect.X, UV2Rect.Y));
	for (int32 v = 0; v < 4; ++v)
	{
		Normals.Add(Normal);
		Colors.Add(Colors4[v]);
		UV1.Add(FVector2D(Alpha4[v], 0.0));		// the sprite masters read the fade from UV1.x
		Tangents.Add(FProcMeshTangent(Tangent, false));
	}
	Triangles.Append({ Base, Base + 2, Base + 1, Base, Base + 3, Base + 2 });
}

// ---- Lifecycle ----------------------------------------------------------------------------------------------------

ASourceParticleSystem::ASourceParticleSystem()
{
	PrimaryActorTick.bCanEverTick = true;
	// After the player has moved, so the billboards face where the camera is this frame, not last.
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Particles"));
	SetRootComponent(Mesh);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->bUseComplexAsSimpleCollision = false;
	// The mesh is in the actor's space so a moving effect moves with it, but the quads are built in world
	// orientation, so the component itself must not turn with the actor.
	Mesh->SetUsingAbsoluteRotation(true);
}

ASourceParticleSystem* ASourceParticleSystem::Create(UWorld* World, const FString& InEffectName, const FVector& Location,
	const FVector3f& SourceAngles, ULambdaMaterialLibrary* Materials, bool bInOneShot)
{
	if (!World)
	{
		return nullptr;
	}
	if (!FSourceParticleLibrary::Get().Find(InEffectName))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Particle effect '%s' is not defined in any loaded .pcf"), *InEffectName);
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ASourceParticleSystem* Actor = World->SpawnActor<ASourceParticleSystem>(ASourceParticleSystem::StaticClass(),
		FTransform(FSourceCoords::AnglesToUE(SourceAngles), Location), Params);
	if (!Actor)
	{
		return nullptr;
	}
	Actor->bOneShot = bInOneShot;
	if (!Actor->SetEffect(InEffectName, Materials))
	{
		Actor->Destroy();
		return nullptr;
	}
	Actor->StartEffect();
	return Actor;
}

bool ASourceParticleSystem::SetEffect(const FString& InEffectName, ULambdaMaterialLibrary* Materials)
{
	Definition = FSourceParticleLibrary::Get().Find(InEffectName);
	if (!Definition)
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("Particle effect '%s' is not defined in any loaded .pcf"), *InEffectName);
		return false;
	}
	EffectName = Definition->Name;
	MaterialLibrary = Materials;
	Root = MakeUnique<FSourceParticleSimulator>(Definition);
	BuildRenderList();
	EmittersEndTime = Root->ComputeEmittersEndTime();
	ApplyWorldHooks();
	bActive = false;
	return true;
}

void ASourceParticleSystem::BuildRenderList()
{
	Systems.Reset();
	SectionMaterials.Reset();
	if (!Root)
	{
		return;
	}
	int32 NextSection = 0;
	Root->ForEachSystem([this, &NextSection](FSourceParticleSimulator& Sim)
	{
		FSystemRender R;
		R.Simulator = &Sim;
		R.Section = NextSection++;
		const FSourceParticleDefinition& Def = Sim.GetDefinition();

		// The first renderer of a kind this draws decides the shape; anything else draws as sprites.
		for (const FSourceParticleOperatorDef& Renderer : Def.Renderers)
		{
			const FSourceDMXElement* E = Renderer.Element;
			if (Renderer.FunctionName.Equals(TEXT("render_animated_sprites"), ESearchCase::IgnoreCase))
			{
				R.Kind = FSystemRender::EKind::Sprites;
				R.AnimationRate = E->GetFloat(TEXT("animation rate"), 0.1f);
				R.bFitLifetime = E->GetBool(TEXT("animation_fit_lifetime"), false);
				R.bRateIsFPS = E->GetBool(TEXT("use animation rate as FPS"), false);
				R.OrientationType = E->GetInt(TEXT("orientation_type"), 0);
				R.OrientationControlPoint = E->GetInt(TEXT("orientation control point"), -1);
				R.SecondSequenceRate = E->GetFloat(TEXT("second sequence animation rate"), 0.0f);
				break;
			}
			if (Renderer.FunctionName.Equals(TEXT("render_sprite_trail"), ESearchCase::IgnoreCase))
			{
				R.Kind = FSystemRender::EKind::Trail;
				R.AnimationRate = E->GetFloat(TEXT("animation rate"), 0.1f);
				R.LengthFadeInTime = E->GetFloat(TEXT("length fade in time"), 0.0f);
				R.MaxLength = E->GetFloat(TEXT("max length"), 2000.0f);
				R.MinLength = E->GetFloat(TEXT("min length"), 0.0f);
				R.bConstrainRadiusToLength = E->GetBool(TEXT("constrain radius to length"), true);
				R.TailScale = E->GetVector4(TEXT("tail color and alpha scale factor"), FVector4f(1, 1, 1, 1));
				break;
			}
			if (Renderer.FunctionName.Equals(TEXT("render_rope"), ESearchCase::IgnoreCase))
			{
				R.Kind = FSystemRender::EKind::Rope;
				R.TexelSize = E->GetFloat(TEXT("texel_size"), 4.0f);
				R.TextureScrollRate = E->GetFloat(TEXT("texture_scroll_rate"), 0.0f);
				R.TextureOffset = E->GetFloat(TEXT("texture_offset"), 0.0f);
				break;
			}
		}

		if (MaterialLibrary)
		{
			FSourceMaterialInfo Info;
			if (MaterialLibrary->LoadMaterialInfo(Def.Material, Info))
			{
				// A Refract material is heat haze - a distortion of what is behind it, which these sprites cannot
				// do. Drawing its normal map as a sprite would be a grey blob, so the system is not drawn at all.
				if (!Info.Shader.Equals(TEXT("Refract"), ESearchCase::IgnoreCase))
				{
					if (UMaterialInterface* Material = MaterialLibrary->GetSpriteMaterial(Def.Material))
					{
						R.MaterialIndex = SectionMaterials.Add(Material);
						R.bAdditive = Info.bAdditive;
						R.StartFadeSize = Info.StartFadeSize;
						R.EndFadeSize = Info.EndFadeSize;
					}
					R.Sheet = MaterialLibrary->GetSpriteSheet(Def.Material);
					if (UTexture2D* Texture = Info.BaseTexture.IsEmpty() ? nullptr : MaterialLibrary->GetTexture(Info.BaseTexture))
					{
						R.TextureHeight = FMath::Max(1, Texture->GetSizeY());
					}
					// "$dualsequence 1": every quad also carries the second sequence's frame (the flames of Half-Life 2's
					// explosions ride on the alpha of their smoke puffs), which the dual sprite master combines.
					R.bDualSequence = Info.bDualSequence && R.Sheet && R.Sheet->Sequences.Num() > 1;
				}
			}
			else
			{
				UE_LOG(LogLambdaSource, Warning, TEXT("Particle '%s': material '%s' not found"), *Def.Name, *Def.Material);
			}
		}

		if (R.Sheet)
		{
			// Lifetime From Sequence wants to know how long each sequence is.
			TArray<int32>& Counts = Sim.GetContext().SheetSequenceFrameCounts;
			Counts.Reset();
			for (const FSourceSheetSequence& Sequence : R.Sheet->Sequences)
			{
				Counts.Add(Sequence.Frames.Num());
			}
		}
		Systems.Add(MoveTemp(R));
	});
	Geometries.SetNum(NextSection);
}

void ASourceParticleSystem::ApplyWorldHooks()
{
	if (!Root)
	{
		return;
	}
	FSourceParticleContext Template;
	TWeakObjectPtr<UWorld> WeakWorld = GetWorld();
	// Traces run against the brushes only (object type WorldStatic here), which is what Source's particles
	// collide with: a particle that bounced off the player or a crate would look wrong and cost a lot.
	Template.TraceLine = [WeakWorld](const FVector3f& Start, const FVector3f& End, FSourceParticleTraceHit& Out) -> bool
	{
		UWorld* World = WeakWorld.Get();
		if (!World)
		{
			return false;
		}
		const float Scale = FSourceCoords::GetUnitScale();
		FHitResult Hit;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(LambdaParticleTrace), /*bTraceComplex=*/ false);
		const bool bHit = World->LineTraceSingleByObjectType(Hit, FSourceCoords::ToUE(Start, Scale), FSourceCoords::ToUE(End, Scale),
			FCollisionObjectQueryParams(ECC_WorldStatic), Params);
		Out.bHit = bHit;
		Out.Fraction = bHit ? Hit.Time : 1.0f;
		if (bHit)
		{
			Out.Position = FSourceCoords::ToSource(Hit.ImpactPoint, Scale);
			Out.Normal = FVector3f((float)Hit.ImpactNormal.X, (float)-Hit.ImpactNormal.Y, (float)Hit.ImpactNormal.Z);
		}
		return true;
	};
	Root->SetWorldHooks(Template);
}

void ASourceParticleSystem::StartEffect()
{
	if (!Root)
	{
		return;
	}
	Root->Reset();
	const float Scale = FSourceCoords::GetUnitScale();
	Root->SetControlPoint(0, FSourceCoords::ToSource(GetActorLocation(), Scale), /*bResetPrevious=*/ true);
	FSourceParticleControlPoint Basis;
	Basis.SetOrientationFromAngles(FSourceCoords::AnglesFromUE(GetActorRotation()));
	Root->SetControlPointOrientation(0, Basis.Forward, Basis.Right, Basis.Up);
	bActive = true;
}

void ASourceParticleSystem::StopEffect(bool bPlayEndCap)
{
	if (Root)
	{
		Root->StopEmission(bPlayEndCap);
	}
}

void ASourceParticleSystem::StopAndDestroy()
{
	if (Root)
	{
		Root->KillAllParticles();
	}
	bActive = false;
	Destroy();
}

void ASourceParticleSystem::SetControlPointLocation(int32 Index, const FVector& Location)
{
	if (Root && Index > 0)
	{
		Root->SetControlPoint(Index, FSourceCoords::ToSource(Location, FSourceCoords::GetUnitScale()), /*bResetPrevious=*/ !bActive);
	}
}

void ASourceParticleSystem::SetControlPointAngles(int32 Index, const FVector3f& SourceAngles)
{
	if (Root && Index > 0)
	{
		FSourceParticleControlPoint Basis;
		Basis.SetOrientationFromAngles(SourceAngles);
		Root->SetControlPointOrientation(Index, Basis.Forward, Basis.Right, Basis.Up);
	}
}

bool ASourceParticleSystem::IsFinished() const
{
	if (!Root || !bActive)
	{
		return true;
	}
	const bool bEmittersDone = Root->IsEmissionStopped() || Root->GetContext().Time > EmittersEndTime + 0.5f;
	return bEmittersDone && Root->GetContext().Time > 0.0f && Root->TotalParticleCount() == 0;
}

int32 ASourceParticleSystem::GetTotalParticleCount() const
{
	return Root ? Root->TotalParticleCount() : 0;
}

FString ASourceParticleSystem::GetDebugString() const
{
	if (!Root)
	{
		return FString::Printf(TEXT("%s: no effect"), *EffectName);
	}
	FString Out = FString::Printf(TEXT("%s at %s: t=%.2f %s%s, %d particles, emitters end %s"), *EffectName,
		*GetActorLocation().ToCompactString(), Root->GetContext().Time, bActive ? TEXT("active") : TEXT("idle"),
		IsFinished() ? TEXT(" finished") : TEXT(""), Root->TotalParticleCount(),
		EmittersEndTime >= FLT_MAX ? TEXT("never") : *FString::Printf(TEXT("%.2f"), EmittersEndTime));
	for (int32 s = 0; s < Systems.Num(); ++s)
	{
		const FSystemRender& System = Systems[s];
		const FSectionGeometry* G = Geometries.IsValidIndex(System.Section) ? &Geometries[System.Section] : nullptr;
		const int32 Verts = G ? G->Vertices.Num() : 0;
		const FString FirstVert = Verts > 0 ? G->Vertices[0].ToCompactString() : TEXT("-");
		const FString FirstColor = Verts > 0 ? G->Colors[0].ToString() : TEXT("-");
		const FString FirstUV = Verts > 0 ? FString::Printf(TEXT("%s a=%.2f%s"), *G->UV0[0].ToString(), G->UV1[0].X,
			System.bDualSequence && G->UV2.Num() > 0 ? *FString::Printf(TEXT(" uv2 %s"), *G->UV2[0].ToString()) : TEXT("")) : TEXT("-");
		Out += FString::Printf(TEXT("\n    %s: %d/%d particles, %d verts, material %s%s%s, sheet %d seq, cp0 %s, v0 %s col %s uv %s"),
			*System.Simulator->GetDefinition().Name, System.Simulator->GetParticles().Num(), System.Simulator->GetParticles().Capacity(), Verts,
			System.MaterialIndex == INDEX_NONE ? TEXT("NONE") : *System.Simulator->GetDefinition().Material,
			System.bAdditive ? TEXT(" additive") : TEXT(""), System.Kind == FSystemRender::EKind::Trail ? TEXT(" trail") : (System.Kind == FSystemRender::EKind::Rope ? TEXT(" rope") : (System.bDualSequence ? TEXT(" dual") : TEXT(""))),
			System.Sheet ? System.Sheet->Sequences.Num() : 0, *System.Simulator->GetContext().CP(0).Position.ToString(),
			*FirstVert, *FirstColor, *FirstUV);
	}
	return Out;
}

void ASourceParticleSystem::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Root || !bActive)
	{
		return;
	}
	UWorld* World = GetWorld();
	const float Scale = FSourceCoords::GetUnitScale();

	// Control point 0 is the actor, wherever it has got to.
	Root->SetControlPoint(0, FSourceCoords::ToSource(GetActorLocation(), Scale), /*bResetPrevious=*/ false);
	FSourceParticleControlPoint Basis;
	Basis.SetOrientationFromAngles(FSourceCoords::AnglesFromUE(GetActorRotation()));
	Root->SetControlPointOrientation(0, Basis.Forward, Basis.Right, Basis.Up);

	// The player, for the operators that follow him.
	{
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		FSourceParticleContext Template = Root->GetContext();
		Template.bHavePlayer = Pawn != nullptr;
		if (Pawn)
		{
			FVector EyeLoc;
			FRotator EyeRot;
			PC->GetPlayerViewPoint(EyeLoc, EyeRot);
			Template.PlayerPosition = FSourceCoords::ToSource(Pawn->GetActorLocation(), Scale);
			Template.PlayerEyeAngles = FSourceCoords::AnglesFromUE(EyeRot);
		}
		Root->ForEachSystem([&Template](FSourceParticleSimulator& System)
		{
			FSourceParticleContext& Ctx = System.GetContext();
			Ctx.bHavePlayer = Template.bHavePlayer;
			Ctx.PlayerPosition = Template.PlayerPosition;
			Ctx.PlayerEyeAngles = Template.PlayerEyeAngles;
		});
	}

	// A long hitch is not simulated through; the sub-stepping caps the cost at ten steps of the maximum.
	Root->Update(FMath::Min(DeltaSeconds, 0.5f));
	RebuildMesh();

	DebugLogTimer -= DeltaSeconds;
	if (DebugLogTimer <= 0.0f)
	{
		DebugLogTimer = 0.5f;
		UE_LOG(LogLambdaSource, Verbose, TEXT("particle %s"), *GetDebugString());
	}

	if (bOneShot && IsFinished())
	{
		Destroy();
	}
}

// ---- Drawing ------------------------------------------------------------------------------------------------------

void ASourceParticleSystem::PackColor(bool bAdditive, const FVector3f& Tint, float Alpha, FLinearColor& OutColor, float& OutAlpha)
{
	// Vertex colours reach the mesh as 8-bit, so nothing above 1 survives here: the material's brightness
	// ($overbrightfactor) is an instance parameter instead.
	const float A = FMath::Clamp(Alpha, 0.0f, 1.0f);
	const FVector3f& Scaled = Tint;
	if (bAdditive)
	{
		// The additive master adds the colour as it is, so the fade has to be in the colour.
		OutColor = FLinearColor(Scaled.X * A, Scaled.Y * A, Scaled.Z * A, 1.0f);
		OutAlpha = 1.0f;
	}
	else
	{
		OutColor = FLinearColor(Scaled.X, Scaled.Y, Scaled.Z, A);
		OutAlpha = A;
	}
}

ASourceParticleSystem::FFrame ASourceParticleSystem::GetFrame(const FSystemRender& System, int32 SequenceNumber, float Age, float Life, float Rate) const
{
	FFrame Frame;
	if (!System.Sheet || System.Sheet->Sequences.Num() == 0)
	{
		return Frame;
	}
	const FSourceSheetSequence& Sequence = *FindSequence(*System.Sheet, SequenceNumber);
	const int32 NumFrames = Sequence.Frames.Num();
	if (NumFrames == 0)
	{
		return Frame;
	}
	if (NumFrames == 1)
	{
		Frame.UV = Frame.NextUV = Sequence.Frames[0].UV;
		return Frame;
	}

	float TotalTime = Sequence.TotalTime;
	if (TotalTime <= 0.0f)
	{
		for (const FSourceSheetFrame& F : Sequence.Frames)
		{
			TotalTime += F.Duration;
		}
	}
	if (TotalTime <= 0.0f)
	{
		Frame.UV = Frame.NextUV = Sequence.Frames[0].UV;
		return Frame;
	}

	// Where in the sequence's own time units the particle is: the rate is cycles per second, or frames per
	// second with the FPS flag, or the whole sequence spread over the particle's life.
	float Units;
	if (System.bFitLifetime && Life > 0.0f)
	{
		Units = Age / Life * TotalTime;
	}
	else if (System.bRateIsFPS)
	{
		Units = Age * Rate;
	}
	else
	{
		Units = Age * Rate * TotalTime;
	}
	if (Sequence.bClamp)
	{
		Units = FMath::Clamp(Units, 0.0f, TotalTime - 1e-4f);
	}
	else
	{
		Units = FMath::Fmod(FMath::Max(0.0f, Units), TotalTime);
	}

	int32 Index = 0;
	float Local = Units;
	while (Index < NumFrames - 1 && Local >= Sequence.Frames[Index].Duration && Sequence.Frames[Index].Duration > 0.0f)
	{
		Local -= Sequence.Frames[Index].Duration;
		++Index;
	}
	const float Duration = Sequence.Frames[Index].Duration;
	Frame.Blend = Duration > 0.0f ? FMath::Clamp(Local / Duration, 0.0f, 1.0f) : 0.0f;
	int32 Next = Index + 1;
	if (Next >= NumFrames)
	{
		if (Sequence.bClamp)
		{
			Next = Index;
			Frame.Blend = 0.0f;
		}
		else
		{
			Next = 0;
		}
	}
	Frame.UV = Sequence.Frames[Index].UV;
	Frame.NextUV = Sequence.Frames[Next].UV;
	return Frame;
}

void ASourceParticleSystem::RebuildMesh()
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC || !Mesh)
	{
		return;
	}
	FFrameView View;
	FRotator CamRot;
	PC->GetPlayerViewPoint(View.CameraLocation, CamRot);
	const FRotationMatrix CamMatrix(CamRot);
	View.CameraRight = CamMatrix.GetUnitAxis(EAxis::Y);
	View.CameraUp = CamMatrix.GetUnitAxis(EAxis::Z);
	View.CameraForward = CamRot.Vector();
	View.Origin = GetActorLocation();
	View.Scale = FSourceCoords::GetUnitScale();
	const float FOV = PC->PlayerCameraManager ? PC->PlayerCameraManager->GetFOVAngle() : 90.0f;
	View.TanHalfFOV = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(FOV, 10.0f, 170.0f) * 0.5f));

	for (FSectionGeometry& Geometry : Geometries)
	{
		Geometry.Reset();
	}
	for (const FSystemRender& System : Systems)
	{
		if (System.MaterialIndex == INDEX_NONE || System.Simulator->GetParticles().Num() == 0)
		{
			continue;
		}
		FSectionGeometry& Geometry = Geometries[System.Section];
		switch (System.Kind)
		{
		case FSystemRender::EKind::Trail: FillTrail(System, View, Geometry); break;
		case FSystemRender::EKind::Rope: FillRope(System, View, Geometry); break;
		default: FillSprites(System, View, Geometry); break;
		}
	}

	// Sections go to the mesh in tree order. UV2 is only filled by the sprite quads; the trail and rope
	// sections leave it empty, which the mesh component takes as none.
	const TArray<FVector2D> NoUV;
	auto Commit = [this, &NoUV](int32 Section, int32 MaterialIndex)
	{
		const FSectionGeometry& Geometry = Geometries[Section];
		if (Geometry.Vertices.Num() == 0 || MaterialIndex == INDEX_NONE)
		{
			Mesh->ClearMeshSection(Section);
			return;
		}
		Mesh->CreateMeshSection_LinearColor(Section, Geometry.Vertices, Geometry.Triangles, Geometry.Normals, Geometry.UV0,
			Geometry.UV1, Geometry.UV2, NoUV, Geometry.Colors, Geometry.Tangents, /*bCreateCollision=*/ false);
		Mesh->SetMaterial(Section, SectionMaterials[MaterialIndex]);
	};
	for (const FSystemRender& System : Systems)
	{
		Commit(System.Section, System.MaterialIndex);
	}
}

void ASourceParticleSystem::FillSprites(const FSystemRender& System, const FFrameView& View, FSectionGeometry& Geometry)
{
	const FSourceParticleCollection& P = System.Simulator->GetParticles();
	const FSourceParticleContext& Ctx = System.Simulator->GetContext();
	const int32 Count = P.Num();

	// orientation_type: 0 faces the camera; 1 stays upright and turns about Z to face it; 2 lies in a control
	// point's plane; 3 lies in the plane of the particle's own normal.
	FVector CPRight = FVector::RightVector, CPUp = FVector::UpVector;
	if (System.OrientationType == 2 && System.OrientationControlPoint >= 0)
	{
		const FSourceParticleControlPoint& CP = Ctx.CP(System.OrientationControlPoint);
		CPRight = SafeNormal(FSourceCoords::ToUEDirection(CP.Right), FVector::RightVector);
		CPUp = SafeNormal(FSourceCoords::ToUEDirection(CP.Up), FVector::UpVector);
	}

	// Back to front when the definition asks for it, which alpha-blended smoke needs and additive glows do not.
	const int32* Order = nullptr;
	if (System.Simulator->GetDefinition().bSortParticles && !System.bAdditive && Count > 1)
	{
		SortScratch.SetNumUninitialized(Count, EAllowShrinking::No);
		SortKeys.SetNumUninitialized(Count, EAllowShrinking::No);
		for (int32 i = 0; i < Count; ++i)
		{
			SortScratch[i] = i;
			SortKeys[i] = (float)FVector::DistSquared(FSourceCoords::ToUE(P.GetVector(EAttr::Xyz, i), View.Scale), View.CameraLocation);
		}
		const TArray<float>& Keys = SortKeys;
		SortScratch.Sort([&Keys](int32 A, int32 B) { return Keys[A] > Keys[B]; });
		Order = SortScratch.GetData();
	}

	// A frame, or two crossfading: the sprite master samples one rectangle, and two quads fading against each
	// other read the same as the blend Source's shader does inside the texture lookup. A dual-sequence
	// material's second frame rides along in UV2, its own crossfade folded into the same two quads.
	auto EmitQuads = [](FSectionGeometry& G, const FFrame& Frame, const FFrame* Frame2, bool bAdditive, const FLinearColor& Color,
		float Alpha, const FVector Corners[4], const FVector& Normal, const FVector& Tangent)
	{
		const bool bBlend = Frame.Blend > 0.02f && Frame.NextUV != Frame.UV;
		const float Weight0 = bBlend ? 1.0f - Frame.Blend : 1.0f;
		const FVector4f UV2First = Frame2 ? ((bBlend || Frame2->Blend <= 0.5f) ? Frame2->UV : Frame2->NextUV) : Frame.UV;
		const FVector4f UV2Second = Frame2 ? Frame2->NextUV : Frame.NextUV;
		{
			FLinearColor C0 = Color;
			float A0 = Alpha;
			if (bBlend)
			{
				if (bAdditive) { C0 *= Weight0; } else { A0 *= Weight0; }
			}
			const FLinearColor Colors4[4] = { C0, C0, C0, C0 };
			float Alpha4[4] = { A0, A0, A0, A0 };
			G.AddQuad(Corners, Frame.UV, UV2First, Colors4, Alpha4, Normal, Tangent);
		}
		if (bBlend)
		{
			FLinearColor C1 = Color;
			float A1 = Alpha;
			if (bAdditive) { C1 *= Frame.Blend; } else { A1 *= Frame.Blend; }
			const FLinearColor Colors4[4] = { C1, C1, C1, C1 };
			float Alpha4[4] = { A1, A1, A1, A1 };
			G.AddQuad(Corners, Frame.NextUV, UV2Second, Colors4, Alpha4, Normal, Tangent);
		}
	};

	const FVector Normal = -View.CameraForward;
	for (int32 Slot = 0; Slot < Count; ++Slot)
	{
		const int32 i = Order ? Order[Slot] : Slot;
		const FVector Centre = FSourceCoords::ToUE(P.GetVector(EAttr::Xyz, i), View.Scale) - View.Origin;
		const float Half = P.GetFloat(EAttr::Radius, i) * View.Scale;
		if (Half <= 0.0f)
		{
			continue;
		}
		// The mirror between the two coordinate systems turns a roll the other way.
		const float Roll = -P.GetFloat(EAttr::Rotation, i);
		const FVector3f Tint = P.GetVector(EAttr::TintRgb, i);
		float RawAlpha = P.GetFloat(EAttr::Alpha, i) * P.GetFloat(EAttr::Alpha2, i);
		// SpriteCard's screen-size fade: a sprite that has grown past a fraction of the screen fades away, so
		// the fireball the player is standing in does not become one flat card over the whole view.
		if (System.EndFadeSize > System.StartFadeSize && System.StartFadeSize > 0.0f)
		{
			const float Distance = FMath::Max(1.0f, (float)FVector::Dist(Centre + View.Origin, View.CameraLocation));
			const float ScreenFraction = Half / (Distance * View.TanHalfFOV);
			RawAlpha *= 1.0f - FMath::Clamp((ScreenFraction - System.StartFadeSize) / (System.EndFadeSize - System.StartFadeSize), 0.0f, 1.0f);
		}
		FLinearColor Color;
		float Alpha;
		PackColor(System.bAdditive, Tint, RawAlpha, Color, Alpha);
		if (RawAlpha <= 0.0f)
		{
			continue;
		}

		FVector Right, Up;
		switch (System.OrientationType)
		{
		case 1:
			Up = FVector::UpVector;
			Right = SafeNormal(FVector::CrossProduct(Up, View.CameraLocation - (Centre + View.Origin)), View.CameraRight);
			break;
		case 2:
			Right = CPRight;
			Up = CPUp;
			break;
		case 3:
		{
			const FVector N = FSourceCoords::ToUEDirection(P.GetVector(EAttr::Normal, i));
			if (N.SizeSquared() > 1e-6)
			{
				const FVector Reference = FMath::Abs(N.Z) > 0.99 ? FVector::ForwardVector : FVector::UpVector;
				Right = SafeNormal(FVector::CrossProduct(Reference, N), FVector::RightVector);
				Up = FVector::CrossProduct(N, Right);
			}
			else
			{
				Right = View.CameraRight;
				Up = View.CameraUp;
			}
			break;
		}
		default:
			Right = View.CameraRight;
			Up = View.CameraUp;
			break;
		}
		const float C = FMath::Cos(Roll), S = FMath::Sin(Roll);
		const FVector R = (Right * C + Up * S) * Half;
		const FVector U = (Up * C - Right * S) * Half;
		const FVector Corners[4] = { Centre - R - U, Centre + R - U, Centre + R + U, Centre - R + U };

		const float Age = Ctx.Time - P.GetFloat(EAttr::CreationTime, i);
		const float Life = P.GetFloat(EAttr::LifeDuration, i);
		const FFrame Frame = GetFrame(System, (int32)P.GetFloat(EAttr::SequenceNumber, i), Age, Life, System.AnimationRate);
		FFrame Frame2;
		if (System.bDualSequence)
		{
			Frame2 = GetFrame(System, (int32)P.GetFloat(EAttr::SequenceNumber1, i), Age, Life, System.SecondSequenceRate);
		}
		EmitQuads(Geometry, Frame, System.bDualSequence ? &Frame2 : nullptr, System.bAdditive, Color, Alpha, Corners, Normal, Right);
	}
}

void ASourceParticleSystem::FillTrail(const FSystemRender& System, const FFrameView& View, FSectionGeometry& Geometry)
{
	// render_sprite_trail: each particle is a quad stretched back along its velocity by TRAIL_LENGTH seconds
	// of movement, growing in over "length fade in time" and clamped to [min, max]; the tail end tinted by the
	// tail scale factor. The quad's width lies across the motion and the view, so it faces the camera.
	const FSourceParticleCollection& P = System.Simulator->GetParticles();
	const FSourceParticleContext& Ctx = System.Simulator->GetContext();
	const float Dt = Ctx.DeltaTime;
	const FVector Normal = -View.CameraForward;
	for (int32 i = 0; i < P.Num(); ++i)
	{
		const FVector3f Head = P.GetVector(EAttr::Xyz, i);
		const FVector3f Velocity = Dt > 0.0f ? (Head - P.GetVector(EAttr::PrevXyz, i)) / Dt : FVector3f::ZeroVector;
		const float Speed = Velocity.Size();
		const float Age = Ctx.Time - P.GetFloat(EAttr::CreationTime, i);

		float Length = Speed * P.GetFloat(EAttr::TrailLength, i);
		if (System.LengthFadeInTime > 0.0f && Age < System.LengthFadeInTime)
		{
			Length *= Age / System.LengthFadeInTime;
		}
		Length = FMath::Clamp(Length, System.MinLength, System.MaxLength);
		float Radius = P.GetFloat(EAttr::Radius, i);
		if (System.bConstrainRadiusToLength)
		{
			Radius = FMath::Min(Radius, Length);
		}
		if (Length < 1e-4f || Radius < 1e-4f)
		{
			continue;
		}
		const FVector3f Direction = Speed > 1e-6f ? Velocity / Speed : FVector3f(0, 0, 1);
		const FVector3f Tail = Head - Direction * Length;

		const FVector HeadUE = FSourceCoords::ToUE(Head, View.Scale);
		const FVector TailUE = FSourceCoords::ToUE(Tail, View.Scale);
		const FVector DirUE = SafeNormal(HeadUE - TailUE, FVector::UpVector);
		FVector Side = FVector::CrossProduct(DirUE, HeadUE - View.CameraLocation);
		if (Side.SizeSquared() < 1e-6)
		{
			Side = FVector::CrossProduct(DirUE, FVector::UpVector);
		}
		Side = SafeNormal(Side, FVector::RightVector) * (Radius * View.Scale);

		FLinearColor Color;
		float Alpha;
		const float RawAlpha = P.GetFloat(EAttr::Alpha, i) * P.GetFloat(EAttr::Alpha2, i);
		const FVector3f Tint = P.GetVector(EAttr::TintRgb, i);
		PackColor(System.bAdditive, Tint, RawAlpha, Color, Alpha);
		FLinearColor TailColor;
		float TailAlpha;
		PackColor(System.bAdditive, FVector3f(Tint.X * System.TailScale.X, Tint.Y * System.TailScale.Y, Tint.Z * System.TailScale.Z),
			RawAlpha * System.TailScale.W, TailColor, TailAlpha);

		const FFrame Frame = GetFrame(System, (int32)P.GetFloat(EAttr::SequenceNumber, i), Age, P.GetFloat(EAttr::LifeDuration, i), System.AnimationRate);
		// Texture V runs along the trail: the top of the frame at the head, the bottom at the tail.
		const FVector Corners[4] = { TailUE - Side - View.Origin, TailUE + Side - View.Origin, HeadUE + Side - View.Origin, HeadUE - Side - View.Origin };
		const FLinearColor Colors4[4] = { TailColor, TailColor, Color, Color };
		float Alpha4[4] = { TailAlpha, TailAlpha, Alpha, Alpha };
		Geometry.AddQuad(Corners, Frame.UV, Frame.UV, Colors4, Alpha4, Normal, DirUE);
	}
}

void ASourceParticleSystem::FillRope(const FSystemRender& System, const FFrameView& View, FSectionGeometry& Geometry)
{
	// render_rope: the particles in order, joined into a ribbon; V tiles by texel_size along it and scrolls.
	// Segments are straight (no Catmull-Rom subdivision) and the control-point scaling flags are not applied.
	const FSourceParticleCollection& P = System.Simulator->GetParticles();
	const FSourceParticleContext& Ctx = System.Simulator->GetContext();
	const int32 Count = P.Num();
	if (Count < 2)
	{
		return;
	}
	const float UnitsPerRepeat = FMath::Max(1e-3f, System.TexelSize) * System.TextureHeight;
	const float Scroll = System.TextureOffset + System.TextureScrollRate * Ctx.Time;
	const FVector4f UV = System.Sheet && System.Sheet->Sequences.Num() > 0 && System.Sheet->Sequences[0].Frames.Num() > 0
		? System.Sheet->Sequences[0].Frames[0].UV : FVector4f(0, 0, 1, 1);
	const FVector Normal = -View.CameraForward;

	auto DirectionAt = [&P, Count](int32 Index, const FVector3f& Fallback) -> FVector3f
	{
		const int32 Prev = FMath::Max(0, Index - 1), Next = FMath::Min(Count - 1, Index + 1);
		const FVector3f Dir = P.GetVector(EAttr::Xyz, Next) - P.GetVector(EAttr::Xyz, Prev);
		return Dir.SizeSquared() > 1e-8f ? Dir.GetSafeNormal() : Fallback;
	};
	auto SideAt = [&View](const FVector& Position, const FVector3f& SourceDir) -> FVector
	{
		const FVector Dir = FSourceCoords::ToUEDirection(SourceDir);
		FVector Side = FVector::CrossProduct(Dir, Position - View.CameraLocation);
		if (Side.SizeSquared() < 1e-6)
		{
			Side = FVector::CrossProduct(Dir, FVector::UpVector);
		}
		return SafeNormal(Side, FVector::RightVector);
	};

	float VPrev = Scroll;
	for (int32 i = 0; i < Count - 1; ++i)
	{
		const FVector3f Start = P.GetVector(EAttr::Xyz, i), End = P.GetVector(EAttr::Xyz, i + 1);
		const FVector3f Segment = End - Start;
		const float SegmentLength = Segment.Size();
		const FVector3f Direction = SegmentLength > 1e-6f ? Segment / SegmentLength : FVector3f(0, 0, 1);
		const float VNext = VPrev + SegmentLength / UnitsPerRepeat;

		const FVector StartUE = FSourceCoords::ToUE(Start, View.Scale), EndUE = FSourceCoords::ToUE(End, View.Scale);
		const FVector SideStart = SideAt(StartUE, DirectionAt(i, Direction)) * (P.GetFloat(EAttr::Radius, i) * View.Scale);
		const FVector SideEnd = SideAt(EndUE, DirectionAt(i + 1, Direction)) * (P.GetFloat(EAttr::Radius, i + 1) * View.Scale);

		FLinearColor C0, C1;
		float A0, A1;
		PackColor(System.bAdditive, P.GetVector(EAttr::TintRgb, i), P.GetFloat(EAttr::Alpha, i) * P.GetFloat(EAttr::Alpha2, i), C0, A0);
		PackColor(System.bAdditive, P.GetVector(EAttr::TintRgb, i + 1), P.GetFloat(EAttr::Alpha, i + 1) * P.GetFloat(EAttr::Alpha2, i + 1), C1, A1);

		const int32 Base = Geometry.Vertices.Num();
		const FVector Verts[4] = { StartUE - SideStart - View.Origin, StartUE + SideStart - View.Origin, EndUE + SideEnd - View.Origin, EndUE - SideEnd - View.Origin };
		const FVector2D UVs[4] = { FVector2D(UV.X, VPrev), FVector2D(UV.Z, VPrev), FVector2D(UV.Z, VNext), FVector2D(UV.X, VNext) };
		const FLinearColor Colors4[4] = { C0, C0, C1, C1 };
		const float Alpha4[4] = { A0, A0, A1, A1 };
		for (int32 v = 0; v < 4; ++v)
		{
			Geometry.Vertices.Add(Verts[v]);
			Geometry.UV0.Add(UVs[v]);
			Geometry.UV1.Add(FVector2D(Alpha4[v], 0.0));
			Geometry.Colors.Add(Colors4[v]);
			Geometry.Normals.Add(Normal);
			Geometry.Tangents.Add(FProcMeshTangent(FSourceCoords::ToUEDirection(Direction), false));
		}
		Geometry.Triangles.Append({ Base, Base + 2, Base + 1, Base, Base + 3, Base + 2 });
		VPrev = VNext;
	}
}
