#include "SourcePropPhysics.h"
#include "LambdaMaterialLibrary.h"
#include "LambdaSourceModule.h"
#include "LambdaSourceSettings.h"
#include "LambdaSoundLibrary.h"
#include "SourceCoordinates.h"
#include "SourceImpactEffects.h"
#include "SourceSoundScript.h"
#include "SourceSurfaceProps.h"
#include "Kismet/GameplayStatics.h"
#include "SourceDamage.h"
#include "SourcePHYFile.h"
#include "SourceStudioModelComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "ProceduralMeshComponent.h"

namespace
{
	/** CGrabController::AttachEntity raises the damping of a carried object so it stops swinging about. */
	constexpr float CARRIED_ANGULAR_DAMPING = 10.0f;
	/**
	 * REDUCED_CARRY_MASS: while an object is held its mass is dropped to a kilogram, "to prevent the player from
	 * adding crazy amounts of energy to the system" - a carried filing cabinet must not be able to shove the
	 * world about. The real mass is put back the moment it is let go, so a thrown crate still flies like a crate.
	 */
	constexpr float REDUCED_CARRY_MASS = 1.0f;
	/** DetachEntity clamps what a released object is moving at: hl2_normspeed * 1.5, and two turns a second. */
	constexpr float RELEASE_MAX_SPEED_UNITS = 190.0f * 1.5f;
	constexpr float RELEASE_MAX_ANGULAR_DEGREES = 2.0f * 360.0f;

	// Phys prop spawnflags (props_shared.h). The rest are about breakage, triggers and the physcannon.
	constexpr int32 SF_PHYSPROP_START_ASLEEP = 0x000001;
	constexpr int32 SF_PHYSPROP_MOTIONDISABLED = 0x000008;
	constexpr int32 SF_PHYSPROP_PREVENT_PICKUP = 0x000200;

	// PhysCollisionSound: below this speed a collision is silent, and impacts are never played closer together
	// than this. The volume reaches full at 320 in/s.
	constexpr float IMPACT_MIN_SPEED_UNITS = 70.0f;
	constexpr float IMPACT_FULL_VOLUME_UNITS = 320.0f;
	constexpr float IMPACT_MIN_INTERVAL = 0.05f;
	/** surfacedata_t::audio.hardThreshold's default: a surface softer than this gets the "impactsoft" sound. */
	constexpr float IMPACT_HARD_THRESHOLD = 0.5f;
	/** m_shadow.maxSpeed / DEFAULT_MAX_ANGULAR: how fast the controller may drag a held object about. */
	constexpr float SHADOW_MAX_SPEED_UNITS = 1000.0f;
	constexpr float SHADOW_MAX_ANGULAR_DEGREES = 360.0f * 10.0f;

	/** CBaseEntity::IsInWorld: MAX_COORD_INTEGER, and the speed past which the engine gives up on an object. */
	constexpr float MAX_COORD_UNITS = 16384.0f;
	constexpr float MAX_SPEED_UNITS = 2000.0f;
}

bool ASourcePropPhysics::IsPropClass(const FString& ClassName)
{
	// prop_physics_override and physics_prop are the same entity under other names (props.cpp).
	return ClassName.Equals(TEXT("prop_physics"), ESearchCase::IgnoreCase)
		|| ClassName.Equals(TEXT("prop_physics_override"), ESearchCase::IgnoreCase)
		|| ClassName.Equals(TEXT("prop_physics_multiplayer"), ESearchCase::IgnoreCase)
		|| ClassName.Equals(TEXT("physics_prop"), ESearchCase::IgnoreCase);
}

ASourcePropPhysics::ASourcePropPhysics(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	Body = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Body"));
	SetRootComponent(Body);
	Body->bUseComplexAsSimpleCollision = false;
	Body->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Body->SetCollisionObjectType(ECC_PhysicsBody);
	Body->SetCollisionResponseToAllChannels(ECR_Block);
	Body->SetCastShadow(false);
	Body->SetGenerateOverlapEvents(false);

	Model = CreateDefaultSubobject<USourceStudioModelComponent>(TEXT("Model"));
	Model->SetupAttachment(Body);
	Model->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Model->SetCastShadow(true);
	Model->SetMobility(EComponentMobility::Movable);
}

void ASourcePropPhysics::InitializeFromEntity(const FSourceEntity& InEntity, ULambdaMaterialLibrary* Materials)
{
	Entity = InEntity;
	MaterialLibrary = Materials;
	const float Scale = ULambdaSourceSettings::Get().UnitScale;

	const FString ModelPath = Entity.Get(TEXT("model"));
	if (ModelPath.IsEmpty() || !Model->SetModel(ModelPath, MaterialLibrary))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("%s: no model ('%s')"), *Entity.ClassName, *ModelPath);
		Destroy();
		return;
	}

	FVector3f Origin = FVector3f::ZeroVector;
	Entity.GetVector(TEXT("origin"), Origin);
	FVector3f Angles = FVector3f::ZeroVector;
	Entity.GetVector(TEXT("angles"), Angles);
	SetActorLocation(FSourceCoords::ToUE(Origin, Scale));
	SetActorRotation(FSourceCoords::AnglesToUE(Angles));

	// The collision is the model's .phy, the same file a ragdoll is built from - a prop is normally one solid.
	FSourcePHYFile Phy;
	FString Error;
	float MassKg = 0.0f;
	int32 NumHulls = 0;
	if (Phy.Load(ModelPath, Scale, &Error) && Phy.GetSolids().Num() > 0)
	{
		for (const FSourcePHYSolid& Solid : Phy.GetSolids())
		{
			for (const TArray<FVector>& Hull : Solid.Hulls)
			{
				Body->AddCollisionConvexMesh(Hull);
				++NumHulls;
			}
			MassKg += Solid.Mass;
			if (SurfaceProp.IsEmpty())
			{
				SurfaceProp = Solid.SurfaceProp;
				SavedLinearDamping = Solid.Damping;
				SavedAngularDamping = Solid.RotDamping;
			}
		}
	}
	else
	{
		// No collision model: fall back to the model's own bounds, so the prop is at least solid.
		UE_LOG(LogLambdaSource, Log, TEXT("%s '%s': %s; using the model's bounds"), *Entity.ClassName, *ModelPath, *Error);
		const FVector3f HullMin = Model->GetModel()->GetHullMin();
		const FVector3f HullMax = Model->GetModel()->GetHullMax();
		const FVector Min = FSourceCoords::ToUE(HullMin, Scale);
		const FVector Max = FSourceCoords::ToUE(HullMax, Scale);
		TArray<FVector> Box;
		for (int32 i = 0; i < 8; ++i)
		{
			Box.Add(FVector((i & 1) ? Max.X : Min.X, (i & 2) ? Max.Y : Min.Y, (i & 4) ? Max.Z : Min.Z));
		}
		Body->AddCollisionConvexMesh(Box);
		MassKg = 20.0f;
	}

	if (UBodySetup* Setup = Body->GetBodySetup())
	{
		// Bullets trace against complex geometry, and this body has no mesh sections; the hulls answer for it.
		Setup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
	}
	Body->RecreatePhysicsState();

	// "Motion disabled" props are scenery until something switches them on; there is no input system here yet, so
	// they stay put. Everything else is simulated.
	const int32 SpawnFlags = static_cast<int32>(Entity.GetFloat(TEXT("spawnflags"), 0.0f));
	bPickupPrevented = (SpawnFlags & SF_PHYSPROP_PREVENT_PICKUP) != 0;
	if ((SpawnFlags & SF_PHYSPROP_MOTIONDISABLED) != 0)
	{
		Body->SetSimulatePhysics(false);
		Body->SetMobility(EComponentMobility::Movable);
	}
	else
	{
		Body->SetSimulatePhysics(true);
	}

	// "massscale" scales what the .phy says the prop weighs.
	const float MassScale = Entity.GetFloat(TEXT("massscale"), 0.0f);
	if (MassScale > 0.0f)
	{
		MassKg *= MassScale;
	}
	Body->SetMassOverrideInKg(NAME_None, FMath::Max(0.1f, MassKg), true);
	Body->SetLinearDamping(SavedLinearDamping);
	Body->SetAngularDamping(FMath::Max(SavedAngularDamping, 0.5f));

	// "inertiascale" makes a prop harder or easier to spin without changing what it weighs.
	const float InertiaScale = Entity.GetFloat(TEXT("inertiascale"), 0.0f);
	if (InertiaScale > 0.0f)
	{
		if (FBodyInstance* BodyInstance = Body->GetBodyInstance())
		{
			BodyInstance->InertiaTensorScale = FVector(FMath::Max(0.5f, InertiaScale));
			BodyInstance->UpdateMassProperties();
		}
	}

	// "Start asleep": the prop does not settle until something disturbs it.
	if ((SpawnFlags & SF_PHYSPROP_START_ASLEEP) != 0)
	{
		Body->PutAllRigidBodiesToSleep();
	}

	const FVector3f HullMin = Model->GetModel()->GetHullMin();
	const FVector3f HullMax = Model->GetModel()->GetHullMax();
	SizeUnits = FMath::Max3(HullMax.X - HullMin.X, HullMax.Y - HullMin.Y, HullMax.Z - HullMin.Z);

	PlaceClearOfWorld(HullMin, HullMax, Scale);

	const FBoxSphereBounds LocalBounds = Body->CalcLocalBounds();
	UE_LOG(LogLambdaSource, Log, TEXT("%s '%s': %.1f kg, %.0f units across, surfaceprop '%s', %d hulls, collision %s +/- %s units"),
		*Entity.ClassName, *ModelPath, GetMass(), SizeUnits, *SurfaceProp, NumHulls,
		*(LocalBounds.Origin / Scale).ToCompactString(), *(LocalBounds.BoxExtent / Scale).ToCompactString());
}

void ASourcePropPhysics::PlaceClearOfWorld(const FVector3f& HullMin, const FVector3f& HullMax, float Scale)
{
	UWorld* World = GetWorld();
	if (!World || !Body)
	{
		return;
	}
	// CreatePhysicsProp sweeps the model's hull and sets the prop down on what it hits, a unit clear of the
	// surface. A prop that starts even slightly inside the floor cannot be pushed back out of it - the world is a
	// one-sided triangle mesh here - and falls straight through, so it is set down before physics is switched on.
	const FVector A = FSourceCoords::ToUE(HullMin, Scale);
	const FVector B = FSourceCoords::ToUE(HullMax, Scale);
	const FVector LocalMin = FVector::Min(A, B);
	const FVector LocalMax = FVector::Max(A, B);
	// A shade under the real hull, so the prop is not left in contact with what it was set down on.
	const FVector Extent = (LocalMax - LocalMin) * 0.5f - FVector(Scale);
	if (Extent.GetMin() <= 0.0f)
	{
		return;
	}
	const FVector Offset = GetActorQuat().RotateVector((LocalMax + LocalMin) * 0.5f);

	const FVector End = GetActorLocation() + Offset;
	const FVector Start = End + FVector(0.0f, 0.0f, (LocalMax.Z - LocalMin.Z) + Scale);

	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(LambdaPropPlace), false, this);
	const bool bHit = World->SweepSingleByChannel(Hit, Start, End, GetActorQuat(), ECC_WorldStatic,
		FCollisionShape::MakeBox(FVector3f(Extent)), Params);
	UE_LOG(LogLambdaSource, Verbose, TEXT("%s '%s': place sweep %s%s, extent %s units, from %s to %s"),
		*Entity.ClassName, *Entity.Get(TEXT("model")), bHit ? TEXT("hit ") : TEXT("missed"),
		bHit ? *GetNameSafe(Hit.GetActor()) : TEXT(""), *(Extent / Scale).ToCompactString(),
		*(FSourceCoords::ToSource(Start, Scale)).ToCompactString(), *(FSourceCoords::ToSource(End, Scale)).ToCompactString());
	if (bHit && !Hit.bStartPenetrating)
	{
		const FVector Lifted = Hit.Location - Offset + FVector(0.0f, 0.0f, Scale);
		UE_LOG(LogLambdaSource, Verbose, TEXT("%s '%s': set down %.1f units clear of the floor"),
			*Entity.ClassName, *Entity.Get(TEXT("model")), (Lifted.Z - GetActorLocation().Z) / Scale);
		SetActorLocation(Lifted, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

float ASourcePropPhysics::GetMass() const
{
	return Body ? Body->GetMass() : 0.0f;
}

UPrimitiveComponent* ASourcePropPhysics::GetPhysicsBody() const
{
	return Body;
}

void ASourcePropPhysics::BeginPlay()
{
	Super::BeginPlay();
	if (Body)
	{
		// Chaos only reports contacts for bodies that ask for them.
		Body->SetNotifyRigidBodyCollision(true);
		Body->OnComponentHit.AddDynamic(this, &ASourcePropPhysics::OnBodyHit);
	}
}

void ASourcePropPhysics::OnBodyHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
	FVector NormalImpulse, const FHitResult& Hit)
{
	UWorld* World = GetWorld();
	if (!World || !Body)
	{
		return;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const float Now = World->GetTimeSeconds();

	// PhysCollisionSound: quiet contacts and rapid re-contacts (a crate settling) make no sound.
	const float SpeedUnits = (NormalImpulse / FMath::Max(Body->GetMass(), KINDA_SMALL_NUMBER)).Size() / Scale;
	if (SpeedUnits < IMPACT_MIN_SPEED_UNITS || Now - LastImpactSoundTime < IMPACT_MIN_INTERVAL)
	{
		return;
	}
	LastImpactSoundTime = Now;

	const FSourceSurfaceProps& Surfaces = FSourceSurfaceProps::Get();
	const FSourceSurfaceProp* Surface = Surfaces.Find(SurfaceProp);
	if (!Surface || Surface->ImpactHardSound.IsEmpty())
	{
		return;
	}

	// PlayImpactSounds: landing on something soft calls for the softer of the two sounds.
	FString SoundScript = Surface->ImpactHardSound;
	if (!Surface->ImpactSoftSound.IsEmpty())
	{
		SourceImpact::FSurfaceHitInfo HitInfo;
		if (SourceImpact::ResolveSurface(Hit, MaterialLibrary, HitInfo))
		{
			const FSourceSurfaceProp* HitSurface = Surfaces.Find(HitInfo.SurfaceProp);
			if (HitSurface && HitSurface->AudioHardnessFactor < IMPACT_HARD_THRESHOLD)
			{
				SoundScript = Surface->ImpactSoftSound;
			}
		}
	}

	// volume = speed squared over 320 squared, so a gentle knock is quiet and a thrown crate is not.
	const float Volume = FMath::Min(1.0f, FMath::Square(SpeedUnits / IMPACT_FULL_VOLUME_UNITS));
	float ScriptVolume = 1.0f;
	float Pitch = 1.0f;
	if (ULambdaSoundWave* Wave = FLambdaSoundCache::Get().CreateWaveResolved(this, SoundScript, false, ScriptVolume, Pitch))
	{
		const FSourceSoundScriptEntry* Entry = FSourceSoundScripts::Get().Find(SoundScript);
		USoundAttenuation* Attenuation = FLambdaSoundCache::Get().GetAttenuationForSoundLevel(Entry ? Entry->SoundLevel : 75.0f);
		UGameplayStatics::SpawnSoundAtLocation(this, Wave, Hit.ImpactPoint, FRotator::ZeroRotator,
			ScriptVolume * Volume, Pitch, 0.0f, Attenuation);
	}
}

void ASourcePropPhysics::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// The model rides the body; nothing else to do unless something is carrying it.
	if (Carrier.IsValid() && !Carrier->IsValidLowLevelFast())
	{
		StopCarry(false);
	}

	// CBaseEntity::IsInWorld: anything that has left the map's coordinate space (fallen out of the level, or been
	// flung at a speed the engine cannot represent) is removed rather than simulated forever.
	if (Body && Body->IsSimulatingPhysics())
	{
		const float Scale = ULambdaSourceSettings::Get().UnitScale;
		const FVector3f Position = FSourceCoords::ToSource(GetActorLocation(), Scale);
		const FVector Velocity = Body->GetPhysicsLinearVelocity() / Scale;
		if (FMath::Max3(FMath::Abs(Position.X), FMath::Abs(Position.Y), FMath::Abs(Position.Z)) >= MAX_COORD_UNITS
			|| FMath::Max3(FMath::Abs(Velocity.X), FMath::Abs(Velocity.Y), FMath::Abs(Velocity.Z)) >= MAX_SPEED_UNITS)
		{
			UE_LOG(LogLambdaSource, Log, TEXT("%s '%s' left the world at %g %g %g; removing it"),
				*Entity.ClassName, *Entity.Get(TEXT("model")), Position.X, Position.Y, Position.Z);
			Destroy();
		}
	}
}

float ASourcePropPhysics::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	// CBaseEntity::VPhysicsTakeDamage: the damage force is applied to the object where it was hit, so a shot prop
	// jumps and spins the way the bullet pushed it. Props have no health here (prop_data is not ported).
	if (!DamageEvent.IsOfType(FSourceDamageEvent::ClassID) || !Body || !Body->IsSimulatingPhysics())
	{
		return 0.0f;
	}
	const FSourceDamageEvent& Info = static_cast<const FSourceDamageEvent&>(DamageEvent);
	// "don't let physics impacts or fire cause objects to move (again)"
	if (Info.DamageForce.IsNearlyZero() || Info.DamageType == SourceDamageType::DMG_GENERIC
		|| (Info.DamageType & SourceDamageType::DMG_NO_PHYSICS_FORCE) != 0)
	{
		return 0.0f;
	}
	FVector Force = Info.DamageForce;
	if (IsHeld() && CarriedMassKg > 0.0f)
	{
		// "if the player is holding the object, use its real mass (player holding reduced the mass)": the blow
		// is scaled by how much lighter the prop is being held at, so a shot moves it as much as it would have
		// done sitting on the floor - and does not knock it out of the player's hands.
		Force *= Body->GetMass() / CarriedMassKg;
	}
	Body->AddImpulseAtLocation(Force, Info.DamagePosition);
	return DamageAmount;
}

void ASourcePropPhysics::StartCarry(APawn* Player)
{
	if (!Body || !Player)
	{
		return;
	}
	Carrier = Player;
	// AttachEntity: gravity stops acting on the held object and its damping goes up, so it hangs steadily in
	// front of the player instead of swinging, and its mass is reduced while it is being carried.
	Body->SetEnableGravity(false);
	Body->SetAngularDamping(CARRIED_ANGULAR_DAMPING);
	CarriedMassKg = Body->GetMass();
	// "1 second until error starts accumulating"
	CarryErrorTime = -1.0f;
	CarryError = 0.0f;
	Body->SetMassOverrideInKg(NAME_None, REDUCED_CARRY_MASS, true);
	// A held prop should not shove the player who is holding it.
	Body->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	Body->WakeAllRigidBodies();
}

void ASourcePropPhysics::StopCarry(bool bThrown)
{
	if (!Body)
	{
		return;
	}
	Carrier.Reset();
	Body->SetEnableGravity(true);
	Body->SetLinearDamping(SavedLinearDamping);
	Body->SetAngularDamping(FMath::Max(SavedAngularDamping, 0.5f));
	Body->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	// The real mass comes back before anything is thrown, so the throw is scaled by what the prop actually weighs.
	if (CarriedMassKg > 0.0f)
	{
		Body->SetMassOverrideInKg(NAME_None, CarriedMassKg, true);
		CarriedMassKg = 0.0f;
	}
	Body->WakeAllRigidBodies();
	if (!bThrown)
	{
		// DetachEntity clears the velocity of an object that is resting against something, so a prop put down
		// gently does not shoot away.
		Body->SetPhysicsLinearVelocity(FVector::ZeroVector);
		Body->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
		return;
	}
	// ClampPhysicsVelocity: carrying something into a wall can build up a lot of speed that should not come out
	// when it is released.
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	FVector Velocity = Body->GetPhysicsLinearVelocity();
	if (Velocity.SizeSquared() > FMath::Square(RELEASE_MAX_SPEED_UNITS * Scale))
	{
		Body->SetPhysicsLinearVelocity(Velocity.GetSafeNormal() * RELEASE_MAX_SPEED_UNITS * Scale);
	}
	FVector Angular = Body->GetPhysicsAngularVelocityInDegrees();
	if (Angular.SizeSquared() > FMath::Square(RELEASE_MAX_ANGULAR_DEGREES))
	{
		Body->SetPhysicsAngularVelocityInDegrees(Angular.GetSafeNormal() * RELEASE_MAX_ANGULAR_DEGREES);
	}
}

bool ASourcePropPhysics::UpdateCarry(const FVector& TargetLocation, const FRotator& TargetRotation, float DeltaSeconds, float MaxErrorUnits)
{
	if (!Body || !Body->IsSimulatingPhysics() || DeltaSeconds <= 0.0f)
	{
		return false;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;

	// CGrabController::UpdateObject gives up when the object is more than flError units out of place - it is stuck
	// on something, or the player has walked it into a wall. ComputeError() is a running average that ignores the
	// first second, so a prop being pulled into place is not mistaken for one that cannot get there.
	const FVector Offset = TargetLocation - Body->GetComponentLocation();
	CarryErrorTime += DeltaSeconds;
	if (CarryErrorTime > 0.0f)
	{
		const float ErrorTime = FMath::Min(CarryErrorTime, 1.0f);
		float Error = Offset.Size() / Scale;
		// Error that the shadow could not have covered even at full speed is halved: the prop is on its way, not stuck.
		if (Error / ErrorTime > SHADOW_MAX_SPEED_UNITS)
		{
			Error *= 0.5f;
		}
		CarryError = (1.0f - ErrorTime) * CarryError + Error * ErrorTime;
		CarryErrorTime = 0.0f;
		if (CarryError > MaxErrorUnits)
		{
			return false;
		}
	}

	// The shadow controller Source attaches drives the object to its target by velocity rather than teleporting
	// it, so it still collides with the world on the way. m_shadow.maxSpeed is 1000.
	const FVector Velocity = FMath::Clamp(Offset.Size() / DeltaSeconds, 0.0f, SHADOW_MAX_SPEED_UNITS * Scale) * Offset.GetSafeNormal();
	Body->SetPhysicsLinearVelocity(Velocity);
	UE_LOG(LogLambdaSource, VeryVerbose, TEXT("carry: at %s, target %s, off %.1f units, error %.1f, v %.0f u/s, awake %d, sim %d"),
		*(FSourceCoords::ToSource(Body->GetComponentLocation(), Scale)).ToCompactString(),
		*(FSourceCoords::ToSource(TargetLocation, Scale)).ToCompactString(),
		Offset.Size() / Scale, CarryError, Velocity.Size() / Scale,
		Body->RigidBodyIsAwake() ? 1 : 0, Body->IsSimulatingPhysics() ? 1 : 0);

	// ...and turns it toward the angles it was picked up at, relative to the player's facing.
	const FQuat Current = Body->GetComponentQuat();
	const FQuat Wanted = TargetRotation.Quaternion();
	const FQuat Delta = (Wanted * Current.Inverse()).GetNormalized();
	FVector Axis;
	float AngleRad;
	Delta.ToAxisAndAngle(Axis, AngleRad);
	if (AngleRad > PI)
	{
		AngleRad -= 2.0f * PI;
	}
	const FVector Angular = Axis * FMath::RadiansToDegrees(AngleRad) / FMath::Max(DeltaSeconds, 0.01f) * 0.25f;
	Body->SetPhysicsAngularVelocityInDegrees(Angular.GetClampedToMaxSize(SHADOW_MAX_ANGULAR_DEGREES));
	return true;
}

