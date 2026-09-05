#include "Gameplay/SourceGrenade.h"

#include "Core/LambdaSourceModule.h"
#include "Core/LambdaSourceSettings.h"
#include "Core/SourceCoordinates.h"
#include "Creatures/SourceGameNPC.h"
#include "Gameplay/SourceDamage.h"
#include "Particles/SourceParticleSystem.h"
#include "Rendering/SourceLightFlash.h"
#include "Rendering/SourceImpactEffects.h"
#include "Materials/SourceDecalScript.h"
#include "Audio/LambdaSoundLibrary.h"
#include "Materials/LambdaMaterialLibrary.h"
#include "Formats/SourceBSPFile.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace
{
	/** grenade_frag.cpp: a blip a second, three a second once the warning time has come. */
	constexpr float BLIP_FREQUENCY = 1.0f;
	constexpr float BLIP_FAST_FREQUENCY = 0.3f;
	/** FRAG_GRENADE_WARN_TIME: how long before it goes off the blips speed up (and the AI is warned). */
	constexpr float WARN_TIME = 1.5f;
	/** GRENADE_COEFFICIENT_OF_RESTITUTION: what is left of the speed after a bounce. */
	constexpr float BOUNCE = 0.2f;
	/** GRENADE_RADIUS: the hull the throw and the sweep below reckon with, in units. */
	constexpr float GRENADE_RADIUS_UNITS = 4.0f;
	/** The player's frag grenade: CWeaponFrag::ThrowGrenade's AngularImpulse(600, random(-1200, 1200), 0). */
	constexpr float THROW_SPIN_PITCH_DEGREES = 600.0f;
	constexpr float THROW_SPIN_YAW_DEGREES = 1200.0f;
	/** SF_PHYSPROP_PREVENT_PICKUP: a live grenade is not something to pick up and carry about. */
	constexpr int32 SF_PHYSPROP_PREVENT_PICKUP = 0x000200;
	/** The flash of the explosion: a warm light that fills the room for a third of a second. */
	constexpr float EXPLOSION_LIGHT_CANDELAS = 10000.0f;
	constexpr float EXPLOSION_LIGHT_RADIUS_UNITS = 400.0f;
	constexpr float EXPLOSION_LIGHT_SECONDS = 0.4f;
	constexpr float EXPLOSION_LIGHT_HEIGHT_UNITS = 24.0f;
	const TCHAR* GRENADE_MODEL = TEXT("models/weapons/w_grenade.mdl");
}

ASourceGrenade::ASourceGrenade(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = true;
}

ASourceGrenade* ASourceGrenade::Throw(UWorld* World, AActor* InThrower, const FVector& Location, const FVector& Velocity,
	float FuseSeconds, float InDamage, float RadiusUnits, ULambdaMaterialLibrary* InMaterials, const FVector& AngularVelocityDegrees)
{
	if (!World)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;
	Params.Owner = InThrower;
	ASourceGrenade* Grenade = World->SpawnActor<ASourceGrenade>(ASourceGrenade::StaticClass(), Location, FRotator::ZeroRotator, Params);
	if (!Grenade)
	{
		return nullptr;
	}

	// CGrenadeFrag::Spawn -> VPhysicsInitNormal: the grenade is a physics object built from its own model's
	// collision, so it tumbles, bounces and rolls like the small heavy thing it is. The prop code does all of
	// that, impact sounds included, from a keyvalue set like the one a mapper would write.
	FSourceEntity Entity;
	Entity.ClassName = TEXT("npc_grenade_frag");
	Entity.Pairs.Add(TPair<FString, FString>(TEXT("classname"), Entity.ClassName));
	Entity.Pairs.Add(TPair<FString, FString>(TEXT("model"), GRENADE_MODEL));
	Entity.Pairs.Add(TPair<FString, FString>(TEXT("spawnflags"), FString::FromInt(SF_PHYSPROP_PREVENT_PICKUP)));
	// The prop code places itself by its keyvalues, so the throw position has to be in them - or the grenade
	// is born at the map origin, which is under the floor as often as not.
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const FVector3f Origin = FSourceCoords::ToSource(Location, Scale);
	Entity.Pairs.Add(TPair<FString, FString>(TEXT("origin"), FString::Printf(TEXT("%f %f %f"), Origin.X, Origin.Y, Origin.Z)));
	Entity.Pairs.Add(TPair<FString, FString>(TEXT("angles"), TEXT("0 0 0")));
	Grenade->InitializeFromEntity(Entity, InMaterials, /*bPlaceClearOfWorld=*/ false);
	if (!IsValid(Grenade))
	{
		return nullptr;	// no model; the prop code has said why and taken the actor away
	}
	UPrimitiveComponent* PhysicsBody = Grenade->GetPhysicsBody();
	if (!PhysicsBody)
	{
		Grenade->Destroy();
		return nullptr;
	}
	// It is small and fast: continuous collision keeps it from passing through a wall between two frames.
	PhysicsBody->SetUseCCD(true);
	// COLLISION_GROUP_WEAPON does not collide with players or NPCs; a grenade bounces off them by its own trace
	// (VPhysicsUpdate, see Tick), so the thrower does not trip over it on the way out either.
	PhysicsBody->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);

	Grenade->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
	Grenade->Thrower = InThrower;
	Grenade->Damage = InDamage;
	Grenade->RadiusCm = RadiusUnits * Scale;
	Grenade->Materials = InMaterials;
	const float Now = World->GetTimeSeconds();
	Grenade->DetonateTime = Now + FuseSeconds;
	Grenade->WarnTime = Grenade->DetonateTime - WARN_TIME;

	// SetVelocity: the throw, and the tumble it leaves the hand with.
	PhysicsBody->SetPhysicsLinearVelocity(Velocity);
	FVector Spin = AngularVelocityDegrees;
	if (Spin.IsNearlyZero())
	{
		Spin = FSourceCoords::ToUEDirection(FVector3f(THROW_SPIN_PITCH_DEGREES,
			FMath::FRandRange(-THROW_SPIN_YAW_DEGREES, THROW_SPIN_YAW_DEGREES), 0.0f));
	}
	PhysicsBody->SetPhysicsAngularVelocityInDegrees(Spin);

	UE_LOG(LogLambdaSource, Verbose, TEXT("grenade thrown from %s at %s units/s by %s"),
		*FSourceCoords::ToSource(Location, Scale).ToString(), *(Velocity / Scale).ToString(), *GetNameSafe(InThrower));
	// Spawn: the first blip is the sound of the pin coming out.
	Grenade->Blip();
	Grenade->NextBlipTime = Now + BLIP_FREQUENCY;
	return Grenade;
}

void ASourceGrenade::Blip()
{
	FLambdaSoundCache::EmitSoundAtLocation(this, TEXT("Grenade.Blip"), GetActorLocation());
}

float ASourceGrenade::GetTimeToDetonation() const
{
	const UWorld* World = GetWorld();
	return World ? FMath::Max(0.0f, DetonateTime - World->GetTimeSeconds()) : 0.0f;
}

void ASourceGrenade::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UWorld* World = GetWorld();
	if (bDetonated || !World || !IsValid(this))
	{
		return;
	}
	const float Now = World->GetTimeSeconds();
	if (Now >= DetonateTime)
	{
		Detonate();
		return;
	}

	UPrimitiveComponent* PhysicsBody = GetPhysicsBody();
	if (PhysicsBody && PhysicsBody->IsSimulatingPhysics())
	{
		// A grenade thrown hard at the floor covers more than its own size between two physics steps, and the
		// world is a one-sided mesh with nothing behind it to push a body back out: continuous collision is asked
		// for in Throw, and this is the net under it. The path from the last place it was known to be clear of
		// the world is swept with a probe smaller than the hull (so lying or rolling on the floor is still
		// "clear"), and if it crossed something it is set back with the hull just touching that surface and
		// bounced the way Source's VPhysicsUpdate bounces it: most of the speed absorbed, the tumble reversed.
		const float Scale = ULambdaSourceSettings::Get().UnitScale;
		const float ProbeRadius = 1.5f * Scale;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(LambdaGrenadeSweep), false, this);
		FCollisionObjectQueryParams ObjectTypes;
		ObjectTypes.AddObjectTypesToQuery(ECC_WorldStatic);
		ObjectTypes.AddObjectTypesToQuery(ECC_WorldDynamic);
		FVector Position = GetActorLocation();
		FHitResult Hit;
		// Only a real crossing counts - the centre of the grenade on the far side of the surface the sweep found -
		// so that resting or rolling on the floor, where the probe brushes it, is left to physics.
		const FVector Velocity = PhysicsBody->GetPhysicsLinearVelocity();
		if (bHaveSafePosition && !Position.Equals(SafePosition, 0.01f) && Velocity.Size() > 50.0f * Scale
			&& World->SweepSingleByObjectType(Hit, SafePosition, Position, FQuat::Identity, ObjectTypes, FCollisionShape::MakeSphere(ProbeRadius), Params)
			&& !Hit.bStartPenetrating
			&& FVector::DotProduct(Position - Hit.ImpactPoint, Hit.ImpactNormal) < -0.5f * Scale)
		{
			Position = Hit.Location + Hit.ImpactNormal * (GRENADE_RADIUS_UNITS * Scale - ProbeRadius);
			PhysicsBody->SetWorldLocation(Position, false, nullptr, ETeleportType::TeleportPhysics);
			if (FVector::DotProduct(Velocity, Hit.ImpactNormal) < 0.0f)
			{
				PhysicsBody->SetPhysicsLinearVelocity((Velocity - 2.0f * Hit.ImpactNormal * FVector::DotProduct(Velocity, Hit.ImpactNormal)) * BOUNCE);
				PhysicsBody->SetPhysicsAngularVelocityInDegrees(PhysicsBody->GetPhysicsAngularVelocityInDegrees() * -0.5f);
			}
			UE_LOG(LogLambdaSource, Verbose, TEXT("grenade went through %s at %.0f units/s (%s -> %s); set back on it at %s, normal %s"),
				*GetNameSafe(Hit.GetActor()), Velocity.Size() / Scale, *FSourceCoords::ToSource(SafePosition, Scale).ToString(),
				*FSourceCoords::ToSource(GetActorLocation(), Scale).ToString(), *FSourceCoords::ToSource(Position, Scale).ToString(),
				*Hit.ImpactNormal.ToString());
		}
		if (!World->OverlapAnyTestByObjectType(Position, FQuat::Identity, ObjectTypes, FCollisionShape::MakeSphere(ProbeRadius), Params))
		{
			SafePosition = Position;
			bHaveSafePosition = true;
		}
	}

	// CGrenadeFrag::VPhysicsUpdate: physics lets it pass through characters, so it looks for one along the way
	// it is going this frame and bounces off it by hand - most of the speed absorbed, the tumble reversed.
	if (PhysicsBody && PhysicsBody->IsSimulatingPhysics())
	{
		const FVector Velocity = PhysicsBody->GetPhysicsLinearVelocity();
		if (!Velocity.IsNearlyZero())
		{
			const FVector Start = GetActorLocation();
			FHitResult Hit;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(LambdaGrenadeCharacter), false, this);
			// Never off the thrower: COLLISION_GROUP_WEAPON does not collide with players at all, and a grenade
			// thrown down at your own feet leaves through the capsule you are standing in. Bouncing it off that
			// leaves it hanging in the air beside you, reflected again every frame until the fuse runs out.
			Params.AddIgnoredActor(Thrower.Get());
			if (World->LineTraceSingleByObjectType(Hit, Start, Start + Velocity * DeltaSeconds,
				FCollisionObjectQueryParams(ECC_Pawn), Params) && Hit.GetActor())
			{
				const FVector Reflected = (Velocity - 2.0f * Hit.ImpactNormal * FVector::DotProduct(Velocity, Hit.ImpactNormal)) * BOUNCE;
				PhysicsBody->SetPhysicsLinearVelocity(Reflected);
				PhysicsBody->SetPhysicsAngularVelocityInDegrees(PhysicsBody->GetPhysicsAngularVelocityInDegrees() * -0.5f);
				// "send a tiny amount of damage so the character will react to getting bonked"
				FSourceDamageEvent Event(0.1f, Hit, Velocity.GetSafeNormal(), UDamageType::StaticClass(), Velocity * PhysicsBody->GetMass(),
					SourceDamageType::DMG_CRUSH, SourceHitGroup::HITGROUP_GENERIC);
				Hit.GetActor()->TakeDamage(0.1f, Event, nullptr, Thrower.Get());
			}
		}
	}

	// DelayThink: the blips, once a second and then three times a second in the last second and a half.
	if (Now > NextBlipTime)
	{
		Blip();
		NextBlipTime = Now + (Now >= WarnTime ? BLIP_FAST_FREQUENCY : BLIP_FREQUENCY);
	}
}

float ASourceGrenade::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	// The blow moves it as it would any prop; and with one point of health (m_iHealth = 1), any damage at all
	// is CBaseGrenade::Event_Killed, which is Detonate.
	const float Result = Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
	if (!bDetonated && DamageAmount > 0.0f)
	{
		Detonate();
	}
	return Result;
}

void ASourceGrenade::Detonate()
{
	bDetonated = true;
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const FVector Centre = GetActorLocation();

	// RadiusDamage: everything within the radius takes damage falling off linearly with distance, and only
	// if the blast can reach it - a wall between is the whole reason cover is worth taking. Source traces
	// from the explosion to each victim and skips the ones it cannot see.
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Victim = *It;
		if (!Victim || Victim == this || !Victim->CanBeDamaged())
		{
			continue;
		}
		// An NPC's grenade does not hurt other NPCs, matching the no-friendly-fire rule its bullets already
		// obey. They still dive from it - the blast being survivable is something they do not know.
		if (Victim->IsA<ASourceGameNPC>() && Thrower.IsValid() && Thrower->IsA<ASourceGameNPC>())
		{
			continue;
		}
		const FVector Target = Victim->GetActorLocation();
		const float Distance = FVector::Dist(Centre, Target);
		if (Distance > RadiusCm)
		{
			continue;
		}

		FHitResult Blocked;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(LambdaBlast), /*bTraceComplex=*/ false, this);
		Params.AddIgnoredActor(Victim);
		if (World->LineTraceSingleByChannel(Blocked, Centre, Target, ECC_Visibility, Params))
		{
			continue;	// something solid is in the way; this one is behind cover
		}

		const float Falloff = 1.0f - (Distance / RadiusCm);
		const float Dealt = Damage * Falloff;
		if (Dealt <= 0.0f)
		{
			continue;
		}
		const FVector Dir = (Target - Centre).GetSafeNormal();
		FHitResult Hit;
		Hit.ImpactPoint = Target;
		Hit.Location = Target;
		FSourceDamageEvent Event(Dealt, Hit, Dir, UDamageType::StaticClass(), Dir * Dealt * 20.0f,
			SourceDamageType::DMG_BLAST, SourceHitGroup::HITGROUP_GENERIC);
		Victim->TakeDamage(Dealt, Event, nullptr, Thrower.Get());
	}

	// CBaseGrenade::Detonate traces 32 units down and Explode pulls the blast a hair out of what it finds, so
	// the effect is born on the floor the grenade lies on rather than half inside it. The same trace is what
	// CEnvExplosion stamps its scorch mark on: a blast in mid-air over nothing leaves no mark, as in Source.
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	FVector EffectOrigin = Centre;
	{
		FHitResult Floor;
		FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(LambdaGrenadeFloor), false, this);
		if (World->LineTraceSingleByChannel(Floor, Centre, Centre - FVector(0.0f, 0.0f, 32.0f * Scale), ECC_Visibility, TraceParams))
		{
			EffectOrigin = Floor.ImpactPoint + Floor.ImpactNormal * (0.6f * Scale);
			// UTIL_DecalTrace( &tr, "Scorch" ): the burn the blast leaves on what it went off against. "Scorch"
			// is a decal group in decals_subrect.txt, so it is picked from before it can be stamped.
			SourceImpact::SpawnDecal(Floor, Materials.Get(), FSourceDecalScript::Get().PickDecalMaterial(TEXT("Scorch")));
		}
	}
	// The explosion itself: Half-Life 2's grenade explosion particle system, the flash of light it throws on the
	// room, and the sounds env_explosion and the grenade each make - the boom, then the debris coming down.
	ASourceParticleSystem::Create(World, TEXT("grenade_explosion_01"), EffectOrigin, FVector3f::ZeroVector, Materials.Get());
	ASourceLightFlash::Create(World, EffectOrigin + FVector(0.0f, 0.0f, EXPLOSION_LIGHT_HEIGHT_UNITS * Scale),
		FLinearColor(FColor(255, 200, 120)), EXPLOSION_LIGHT_CANDELAS, EXPLOSION_LIGHT_RADIUS_UNITS * Scale,
		EXPLOSION_LIGHT_SECONDS, /*bCastShadows=*/ true);
	FLambdaSoundCache::EmitSoundAtLocation(World, TEXT("BaseExplosionEffect.Sound"), EffectOrigin);
	FLambdaSoundCache::EmitSoundAtLocation(World, TEXT("BaseGrenade.Explode"), EffectOrigin);

	UE_LOG(LogLambdaSource, Log, TEXT("grenade detonated at %s lying %s (%.0f damage, %.0f cm)"),
		*Centre.ToCompactString(), *GetActorRotation().ToCompactString(), Damage, RadiusCm);
	Destroy();
}

ASourceGrenade* ASourceGrenade::FindLiveGrenadeNear(const UWorld* World, const FVector& Position, float RadiusCm,
	const AActor* Ignoring)
{
	if (!World)
	{
		return nullptr;
	}
	ASourceGrenade* Nearest = nullptr;
	float NearestDist = RadiusCm;
	for (TActorIterator<ASourceGrenade> It(const_cast<UWorld*>(World)); It; ++It)
	{
		ASourceGrenade* Grenade = *It;
		if (!Grenade || Grenade->bDetonated || Grenade->GetThrower() == Ignoring)
		{
			continue;	// nobody runs from his own grenade
		}
		const float Distance = FVector::Dist(Grenade->GetActorLocation(), Position);
		if (Distance < NearestDist)
		{
			NearestDist = Distance;
			Nearest = Grenade;
		}
	}
	return Nearest;
}
