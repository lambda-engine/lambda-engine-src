#include "LambdaPlayerPawn.h"

#include "Core/LambdaSourceSettings.h"
#include "LambdaPlayerMovement.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"

ALambdaPlayerPawn::ALambdaPlayerPawn(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = true;

	// VEC_HULL_MIN/MAX: 32 wide, 72 tall. The settings carry the numbers so a mod can change them, but the
	// shape is fixed - it is a box because Source's is.
	const ULambdaSourceSettings* Settings = GetDefault<ULambdaSourceSettings>();
	const float Scale = Settings ? Settings->UnitScale : 1.905f;
	const float HalfWidth = (Settings ? Settings->PlayerCapsuleRadiusUnits : 16.0f) * Scale;
	const float HalfHeight = (Settings ? Settings->PlayerCapsuleHalfHeightUnits : 36.0f) * Scale;

	Hull = CreateDefaultSubobject<UBoxComponent>(TEXT("Hull"));
	Hull->SetBoxExtent(FVector(HalfWidth, HalfWidth, HalfHeight));
	Hull->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);
	Hull->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	// Source's player is a physics shadow: it pushes what it walks into at its own speed and no harder, so
	// Unreal's own impulses stay off.
	Hull->SetSimulatePhysics(false);
	SetRootComponent(Hull);

	Movement = CreateDefaultSubobject<ULambdaPlayerMovement>(TEXT("PlayerMovement"));
	Movement->SetUpdatedComponent(Hull);
	Movement->SetHullComponent(Hull);

	// The hull is axis aligned and stays that way. Source's player is SOLID_BBOX: turning changes where you
	// look, never the shape you occupy. The box is this pawn's root, so letting the controller yaw the pawn
	// would yaw the box with it - and a box that turns while pressed against a wall turns into the wall.
	// Where the player is facing is the control rotation's business, and the camera's.
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;
}

void ALambdaPlayerPawn::BeginPlay()
{
	Super::BeginPlay();
	if (Movement)
	{
		Movement->OnLanded.AddUObject(this, &ALambdaPlayerPawn::HandleLanded);
	}
}

UPawnMovementComponent* ALambdaPlayerPawn::GetMovementComponent() const
{
	return Movement;
}

float ALambdaPlayerPawn::GetHullHalfHeight() const
{
	return Hull ? Hull->GetScaledBoxExtent().Z : 0.0f;
}

FVector ALambdaPlayerPawn::GetFeetLocation() const
{
	return GetActorLocation() - FVector(0.0f, 0.0f, GetHullHalfHeight());
}

void ALambdaPlayerPawn::Jump()
{
	if (Movement)
	{
		Movement->Jump();
	}
}

void ALambdaPlayerPawn::Crouch()
{
	if (Movement)
	{
		Movement->SetWantsToDuck(true);
	}
}

void ALambdaPlayerPawn::UnCrouch()
{
	if (Movement)
	{
		Movement->SetWantsToDuck(false);
	}
}

bool ALambdaPlayerPawn::IsCrouched() const
{
	return Movement && Movement->IsDucked();
}

void ALambdaPlayerPawn::HandleLanded(const FHitResult& Hit)
{
	Landed(Hit);
}
