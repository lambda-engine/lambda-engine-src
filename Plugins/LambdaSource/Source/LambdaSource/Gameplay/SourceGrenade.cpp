#include "Gameplay/SourceGrenade.h"

#include "Core/LambdaSourceModule.h"
#include "Core/LambdaSourceSettings.h"
#include "Gameplay/SourceDamage.h"
#include "Rendering/SourceStudioModelComponent.h"
#include "Components/SphereComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "EngineUtils.h"

ASourceGrenade::ASourceGrenade()
{
	PrimaryActorTick.bCanEverTick = true;

	USphereComponent* Sphere = CreateDefaultSubobject<USphereComponent>(TEXT("Body"));
	// GRENADE_RADIUS is 4 inches in weapon_frag.cpp; a little over 10 cm.
	Sphere->InitSphereRadius(10.0f);
	Sphere->SetCollisionProfileName(TEXT("BlockAllDynamic"));
	// It must not stop the bullets flying over it, or a grenade on the floor would eat a firefight.
	Sphere->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
	SetRootComponent(Sphere);

	Model = CreateDefaultSubobject<USourceStudioModelComponent>(TEXT("Model"));
	Model->SetupAttachment(RootComponent);
	Model->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	Movement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("Movement"));
	Movement->bShouldBounce = true;
	Movement->Bounciness = 0.2f;			// a grenade lands and stays roughly where it lands
	Movement->Friction = 0.6f;
	Movement->ProjectileGravityScale = 1.0f;
	Movement->UpdatedComponent = Sphere;
}

ASourceGrenade* ASourceGrenade::Throw(UWorld* World, AActor* InThrower, const FVector& Location, const FVector& Velocity,
	float FuseSeconds, float InDamage, float RadiusUnits, ULambdaMaterialLibrary* Materials)
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

	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	Grenade->Thrower = InThrower;
	Grenade->Damage = InDamage;
	Grenade->RadiusCm = RadiusUnits * Scale;
	Grenade->DetonateTime = World->GetTimeSeconds() + FuseSeconds;
	Grenade->Movement->Velocity = Velocity;
	// Whoever threw it does not trip over it on the way out.
	if (InThrower)
	{
		if (UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(Grenade->GetRootComponent()))
		{
			Body->IgnoreActorWhenMoving(InThrower, true);
		}
	}
	if (Grenade->Model)
	{
		Grenade->Model->SetModel(TEXT("models/weapons/w_grenade.mdl"), Materials);
	}
	UGameplayStatics::PlaySoundAtLocation(World, nullptr, Location);
	return Grenade;
}

void ASourceGrenade::BeginPlay()
{
	Super::BeginPlay();
	if (Movement)
	{
		Movement->OnProjectileBounce.AddDynamic(this, &ASourceGrenade::OnBounce);
	}
}

void ASourceGrenade::OnBounce(const FHitResult& Hit, const FVector& Velocity)
{
	// Source plays a bounce sound and keeps the timer running; a frag grenade is not a contact grenade.
	UGameplayStatics::PlaySoundAtLocation(GetWorld(), nullptr, GetActorLocation());
}

float ASourceGrenade::GetTimeToDetonation() const
{
	const UWorld* World = GetWorld();
	return World ? FMath::Max(0.0f, DetonateTime - World->GetTimeSeconds()) : 0.0f;
}

void ASourceGrenade::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bDetonated && GetWorld() && GetWorld()->GetTimeSeconds() >= DetonateTime)
	{
		Detonate();
	}
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

	UE_LOG(LogLambdaSource, Log, TEXT("grenade detonated at %s (%.0f damage, %.0f cm)"),
		*Centre.ToCompactString(), Damage, RadiusCm);
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
