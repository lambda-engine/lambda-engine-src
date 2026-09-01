#include "Creatures/SourceGameNPC.h"

#include "Core/LambdaSourceModule.h"
#include "Core/LambdaSourceSettings.h"
#include "Core/SourceCoordinates.h"
#include "Audio/LambdaSoundLibrary.h"
#include "Game/LambdaGameDll.h"
#include "Gameplay/SourceDamage.h"
#include "Rendering/SourceImpactEffects.h"
#include "Weapons/SourceAmmoDef.h"
#include "World/SourceBSPWorldActor.h"
#include "Components/AudioComponent.h"
#include "Components/CapsuleComponent.h"
#include "NavigationSystem.h"
#include "Kismet/GameplayStatics.h"

namespace
{
	/**
	 * What each mind-driven classname looks like. The mind decides everything the NPC does; what it is made
	 * of is the body's business, the same way a brush entity's geometry belongs to the engine. Growing this
	 * table is how a new mind-driven NPC gets a body.
	 */
	struct FGameNPCAppearance
	{
		const TCHAR* ClassName;
		const TCHAR* Model;
		const TCHAR* HealthSkillKey;
		float DefaultHealth;
		float HullHalfWidthUnits;
		float HullHeightUnits;
		float EyeHeightUnits;
	};

	const FGameNPCAppearance GAppearances[] =
	{
		// HL2's soldier hull is 26x72 with eyes at 64 (CNPC_Combine sets a human hull).
		{ TEXT("npc_combine_soldier"), TEXT("models/combine_soldier.mdl"), TEXT("sk_combine_s_health"), 50.0f, 13.0f, 72.0f, 64.0f },
	};

	const FGameNPCAppearance* FindAppearance(const FString& ClassName)
	{
		for (const FGameNPCAppearance& A : GAppearances)
		{
			if (ClassName.Equals(A.ClassName, ESearchCase::IgnoreCase))
			{
				return &A;
			}
		}
		return nullptr;
	}
}

ASourceGameNPC::ASourceGameNPC(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	Voice = CreateDefaultSubobject<UAudioComponent>(TEXT("Voice"));
	Voice->SetupAttachment(RootComponent);
	Voice->bAutoActivate = false;
}

bool ASourceGameNPC::KnowsAppearanceOf(const FString& ClassName)
{
	return FindAppearance(ClassName) != nullptr;
}

void ASourceGameNPC::InitializeFromEntity(const FSourceEntity& InEntity, ASourceBSPWorldActor* InWorldActor,
	ULambdaMaterialLibrary* Materials)
{
	// The base builds the body (it calls Spawn below); the mind is made after it, because the first thing a
	// mind does is ask its body questions.
	Super::InitializeFromEntity(InEntity, InWorldActor, Materials);

	Behaviour = FLambdaGameDll::Get().CreateEntity(InEntity.ClassName, this, GameId);
	if (!Behaviour)
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("%s: the game module claimed this class but made no entity for it"),
			*InEntity.ClassName);
		return;
	}
	Behaviour->Spawn();
}

void ASourceGameNPC::Spawn()
{
	const FGameNPCAppearance* Look = FindAppearance(Entity.ClassName);
	if (!Look)
	{
		return;
	}
	SetModel(Look->Model);
	SetHull(Look->HullHalfWidthUnits, Look->HullHeightUnits);
	MaxHealth = Health = FSourceAmmoDef::Get().GetSkillValue(Look->HealthSkillKey, Look->DefaultHealth);
	ViewOffsetUnits = FVector3f(0, 0, Look->EyeHeightUnits);
	BloodColor = ESourceBloodColor::Red;
	FieldOfView = 0.4f;			// CNPC_Combine: a soldier scans a little wider than the 0.5 default
	RegisterAsNavInvoker();
	SetActivity(TEXT("ACT_IDLE"));
}

void ASourceGameNPC::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Behaviour)
	{
		Behaviour->Destroy();
		FLambdaGameDll::Get().DestroyEntity(Behaviour, GameId);
		Behaviour = nullptr;
		GameId = lambda::InvalidEntity;
	}
	Super::EndPlay(Reason);
}

void ASourceGameNPC::NPCThink()
{
	// Walk the move the mind asked for. Arrival tolerance is deliberately generous: a tactical position is a
	// place to fight from, not a mark to hit, and an NPC shuffling onto an exact spot reads as robotic.
	if (bMoveActive)
	{
		const float Scale = ULambdaSourceSettings::Get().UnitScale;
		const float ArriveCm = 24.0f * Scale;
		if (FVector::Dist2D(GetActorLocation(), MoveGoal) <= ArriveCm)
		{
			bMoveActive = false;
			StopMoving();
		}
		else if (IsMovementBlocked())
		{
			// Stuck is over, not pending: the mind hears "done", sees where it stands, and plans again.
			bMoveActive = false;
			StopMoving();
		}
		else
		{
			NavigateTo(MoveGoal);
		}
	}

	if (Behaviour)
	{
		Behaviour->Think(ThinkInterval);
	}
}

void ASourceGameNPC::OnTakeDamage_Alive(float Damage, AActor* Attacker, const FSourceDamageEvent& Info)
{
	Super::OnTakeDamage_Alive(Damage, Attacker, Info);
	if (Behaviour && Health > 0.0f)
	{
		Behaviour->OnDamaged(FLambdaGameDll::Get().IdForEntity(Attacker), Damage);
	}
}

void ASourceGameNPC::Event_Killed(AActor* Attacker)
{
	// The dying line is the body's: by the time it is true, there is no mind left to request it.
	EmitSound(TEXT("NPC_CombineGrunt.Die"));

	// The body dies on its own - ragdoll, sounds, cleanup - and the mind goes quiet first so no order arrives
	// for a corpse. The squad learns from the silence: a dead member stops answering GetHealth.
	if (Behaviour)
	{
		Behaviour->Destroy();
		FLambdaGameDll::Get().DestroyEntity(Behaviour, GameId);
		Behaviour = nullptr;
		GameId = lambda::InvalidEntity;
	}
	if (Voice)
	{
		Voice->Stop();
	}
	Super::Event_Killed(Attacker);
}

void ASourceGameNPC::OnMovementBlocked()
{
	// Handled in NPCThink, where the move is ended; nothing to do at the moment of contact.
}

bool ASourceGameNPC::MindMoveTo(const FVector& Goal)
{
	MoveGoal = Goal;
	bMoveActive = true;
	if (!NavigateTo(Goal))
	{
		UE_LOG(LogLambdaSource, Log, TEXT("%s: no route to %s"), *Entity.ClassName, *Goal.ToCompactString());
		bMoveActive = false;
		StopMoving();
		return false;
	}
	return true;
}

void ASourceGameNPC::MindStopMoving()
{
	bMoveActive = false;
	StopMoving();
}

void ASourceGameNPC::MindShootAt(const FVector& TargetPoint, AActor* TargetActor, const lambda::NPCShotParams& Params)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FVector Muzzle = EyePosition();
	if (Params.FireSound && Params.FireSound[0])
	{
		EmitSound(ANSI_TO_TCHAR(Params.FireSound));
	}

	// CBaseEntity::FireBullets, aimed at a point instead of down a view: the spread cone perturbs the shot in
	// the plane perpendicular to the aim, once per pellet.
	const FVector Aim = (TargetPoint - Muzzle).GetSafeNormal();
	const float SpreadSin = FMath::Sin(FMath::DegreesToRadians(FMath::Max(0.0f, Params.SpreadDegrees) * 0.5f));
	const FRotator AimRot = Aim.Rotation();
	const FVector Right = FRotationMatrix(AimRot).GetUnitAxis(EAxis::Y);
	const FVector Up = FRotationMatrix(AimRot).GetUnitAxis(EAxis::Z);
	const float Scale = ULambdaSourceSettings::Get().UnitScale;

	const int32 Pellets = FMath::Max(1, Params.Pellets);
	for (int32 Pellet = 0; Pellet < Pellets; ++Pellet)
	{
		const float X = FMath::FRandRange(-0.5f, 0.5f) + FMath::FRandRange(-0.5f, 0.5f);
		const float Y = FMath::FRandRange(-0.5f, 0.5f) + FMath::FRandRange(-0.5f, 0.5f);
		const FVector Dir = (Aim + Right * (X * SpreadSin) + Up * (Y * SpreadSin)).GetSafeNormal();
		const FVector End = Muzzle + Dir * 56756.0f * Scale;

		FHitResult Hit;
		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(LambdaNPCBullet), /*bTraceComplex=*/ true, this);
		QueryParams.bReturnFaceIndex = true;
		int32 HitGroup = SourceHitGroup::HITGROUP_GENERIC;
		if (!SourceImpact::TraceBullet(World, Muzzle, End, QueryParams, Hit, HitGroup))
		{
			continue;
		}
		SourceImpact::PlayImpact(Hit, MaterialLibrary, this, Dir, Params.DamagePerPellet);
		if (AActor* HitActor = Hit.GetActor())
		{
			const FVector Force = Dir * SourceDamage::BulletImpulse(3.0f, 1200.0f) * 2.54f;
			FSourceDamageEvent DamageEvent(Params.DamagePerPellet, Hit, Dir, UDamageType::StaticClass(), Force,
				SourceDamageType::DMG_BULLET, HitGroup);
			HitActor->TakeDamage(Params.DamagePerPellet, DamageEvent, GetController(), this);
		}
	}
	(void)TargetActor;
}

bool ASourceGameNPC::MindSpeak(const FString& Soundscript)
{
	if (!Voice)
	{
		return false;
	}
	float Volume = 1.0f, Pitch = 1.0f;
	ULambdaSoundWave* Wave = FLambdaSoundCache::Get().CreateWaveResolved(this, Soundscript, false, Volume, Pitch);
	if (!Wave)
	{
		return false;
	}
	// One mouth: a new line cuts the old one. Deciding which line matters more was the mind's job before the
	// request got here.
	UE_LOG(LogLambdaSource, Log, TEXT("%s says %s"), *Entity.ClassName, *Soundscript);
	Voice->Stop();
	Voice->SetSound(Wave);
	Voice->SetVolumeMultiplier(Volume);
	Voice->SetPitchMultiplier(Pitch);
	Voice->Play();
	return true;
}

bool ASourceGameNPC::MindIsSpeaking() const
{
	return Voice && Voice->IsPlaying();
}

// ---------------------------------------------------------------------------------------------------------
// Tactics from geometry.
//
// F.E.A.R. read its tactics from nodes a designer placed; there is no designer here, so the level itself is
// the tactical map. Candidate positions come off the navmesh in rings around the asker, and the world's own
// collision answers whether each one hides from - or sees - the threat. The threat moving changes the
// answers, which is exactly the point: cover is a relation between three things, not a property of a spot.
// ---------------------------------------------------------------------------------------------------------

namespace
{
	/** A blocking trace between two points, seeing only the world - pawns and debris are not cover. */
	bool WorldBlocks(const UWorld* World, const FVector& From, const FVector& To)
	{
		FHitResult Hit;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(LambdaCoverTrace), /*bTraceComplex=*/ true);
		Params.AddIgnoredActors(TArray<const AActor*>());
		return World->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Params)
			&& Hit.GetActor() && !Hit.GetActor()->IsA<APawn>();
	}
}

bool ASourceGameNPC::IsPointCoverFrom(const FVector& Pos, const FVector& ThreatPos) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	// The threat shoots from its eyes (a player's are at 64 units); cover protects the chest of a man
	// crouched behind it (36 units). If the world blocks that line, the spot is cover.
	const FVector ThreatEyes = ThreatPos + FVector(0, 0, 64.0f * Scale);
	const FVector Chest = Pos + FVector(0, 0, 36.0f * Scale);
	return WorldBlocks(World, ThreatEyes, Chest);
}

bool ASourceGameNPC::MindFindCover(const FVector& ThreatPos, float MinDistCm, float MaxDistCm, FVector& OutPos)
{
	UWorld* World = GetWorld();
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!World || !Nav)
	{
		return false;
	}

	const FVector Me = GetActorLocation();
	const FVector AwayFromThreat = (Me - ThreatPos).GetSafeNormal2D();

	// Rings of candidates around the NPC, nearest ring first, directions biased away from the threat. First
	// acceptable answer wins: F.E.A.R. did not shop for the best cover either, it took workable cover fast -
	// an AI that dithers over optimality reads as stupid, one that commits reads as decisive.
	const int32 Directions = 12;
	const int32 Rings = 4;
	for (int32 Ring = 0; Ring < Rings; ++Ring)
	{
		const float Radius = FMath::Lerp(MinDistCm, MaxDistCm, (Ring + 0.5f) / Rings);
		for (int32 Step = 0; Step < Directions; ++Step)
		{
			// 0, +30, -30, +60, -60... so the search fans outward from "directly away".
			const float SignedIndex = (Step % 2 == 0) ? (Step / 2) : -(Step / 2 + 1);
			const float Angle = SignedIndex * (360.0f / Directions);
			const FVector Dir = AwayFromThreat.RotateAngleAxis(Angle, FVector::UpVector);
			const FVector Candidate = Me + Dir * Radius;

			FNavLocation Projected;
			// The vertical extent must stay well under the room height, or the projection helpfully finds
			// the navmesh on top of the ceiling and every plan tries to walk there.
			if (!Nav->ProjectPointToNavigation(Candidate, Projected, FVector(150.0f, 150.0f, 180.0f)))
			{
				continue;
			}
			if (FVector::Dist2D(Projected.Location, ThreatPos) < MinDistCm)
			{
				continue;	// hiding at the threat's feet is not hiding
			}
			if (!IsPointCoverFrom(Projected.Location, ThreatPos))
			{
				continue;
			}
			UE_LOG(LogLambdaSource, Verbose, TEXT("%s: cover at %s"), *Entity.ClassName, *Projected.Location.ToCompactString());
			OutPos = Projected.Location;
			return true;
		}
	}
	return false;
}

bool ASourceGameNPC::MindFindFlank(const FVector& ThreatPos, float MinDistCm, float MaxDistCm, FVector& OutPos)
{
	UWorld* World = GetWorld();
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!World || !Nav)
	{
		return false;
	}

	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const FVector Me = GetActorLocation();
	const FVector ThreatEyes = ThreatPos + FVector(0, 0, 64.0f * Scale);
	const FVector MyBearing = (Me - ThreatPos).GetSafeNormal2D();

	// A flank is judged from the threat's side of the relation: somewhere that sees it from an angle well
	// off the one it is already being seen from. Sixty degrees is where "the same firefight" becomes "a
	// second front".
	const int32 Directions = 16;
	const int32 Rings = 3;
	for (int32 Ring = 0; Ring < Rings; ++Ring)
	{
		const float Radius = FMath::Lerp(MinDistCm, MaxDistCm, (Ring + 0.5f) / Rings);
		for (int32 Step = 0; Step < Directions; ++Step)
		{
			const float Angle = Step * (360.0f / Directions);
			const FVector Dir = MyBearing.RotateAngleAxis(Angle, FVector::UpVector);
			const FVector Candidate = ThreatPos + Dir * Radius;

			const float BearingDelta = FMath::RadiansToDegrees(
				FMath::Acos(FVector::DotProduct(Dir.GetSafeNormal2D(), MyBearing)));
			if (BearingDelta < 60.0f)
			{
				continue;	// still the side it is already watching
			}

			FNavLocation Projected;
			if (!Nav->ProjectPointToNavigation(Candidate, Projected, FVector(150.0f, 150.0f, 180.0f)))
			{
				continue;
			}
			if (FVector::Dist2D(Projected.Location, Me) < 150.0f)
			{
				continue;	// a flank is a movement; where I already stand is not one
			}
			const FVector CandidateEyes = Projected.Location + FVector(0, 0, ViewOffsetUnits.Z * Scale);
			if (WorldBlocks(World, CandidateEyes, ThreatEyes))
			{
				continue;	// a flank that cannot see the threat flanks nothing
			}
			OutPos = Projected.Location;
			return true;
		}
	}
	return false;
}
