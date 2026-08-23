#include "LambdaCharacter.h"
#include "LambdaEngine.h"
#include "LambdaSourceSettings.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "Kismet/KismetSystemLibrary.h"
#include "SourceBrushEntity.h"
#include "LambdaSourceSettings.h"
#include "Engine/HitResult.h"

ALambdaCharacter::ALambdaCharacter()
{
	PrimaryActorTick.bCanEverTick = false;

	const ULambdaSourceSettings* Settings = GetDefault<ULambdaSourceSettings>();
	const float Scale = Settings ? Settings->UnitScale : 1.905f;
	const float RadiusCm = (Settings ? Settings->PlayerCapsuleRadiusUnits : 16.0f) * Scale;
	const float HalfHeightCm = (Settings ? Settings->PlayerCapsuleHalfHeightUnits : 36.0f) * Scale;
	const float EyeHeightCm = (Settings ? Settings->PlayerEyeHeightUnits : 64.0f) * Scale;

	GetCapsuleComponent()->InitCapsuleSize(RadiusCm, HalfHeightCm);
	BaseEyeHeight = EyeHeightCm - HalfHeightCm;
	CrouchedEyeHeight = BaseEyeHeight * 0.5f;

	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = true;
	bUseControllerRotationRoll = false;

	FirstPersonCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
	FirstPersonCamera->SetupAttachment(GetCapsuleComponent());
	FirstPersonCamera->SetRelativeLocation(FVector(0.0f, 0.0f, EyeHeightCm - HalfHeightCm));
	FirstPersonCamera->bUsePawnControlRotation = true;
	FirstPersonCamera->FieldOfView = 90.0f;

	// No visible body for the POC.
	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		MeshComp->SetVisibility(false);
	}

	ApplySourceMovementSettings();
}

void ALambdaCharacter::ApplySourceMovementSettings()
{
	const ULambdaSourceSettings* Settings = GetDefault<ULambdaSourceSettings>();
	const float Scale = Settings ? Settings->UnitScale : 1.905f;
	const float WalkUnits = Settings ? Settings->PlayerWalkSpeedUnits : 190.0f;
	const float SprintUnits = Settings ? Settings->PlayerSprintSpeedUnits : 320.0f;
	const float JumpHeightUnits = Settings ? Settings->PlayerJumpHeightUnits : 21.0f;
	const float GravityUnits = Settings ? Settings->GravityUnits : 600.0f;
	const float StepUnits = Settings ? Settings->StepHeightUnits : 18.0f;

	WalkSpeedCm = WalkUnits * Scale;
	SprintSpeedCm = SprintUnits * Scale;
	const float GravityCm = GravityUnits * Scale;

	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->MaxWalkSpeed = WalkSpeedCm;
	Move->MaxWalkSpeedCrouched = WalkSpeedCm * 0.33f;
	Move->MaxStepHeight = StepUnits * Scale;
	Move->SetWalkableFloorZ(0.7f);								// Source: surfaces with normal.z < 0.7 are not walkable
	Move->JumpZVelocity = FMath::Sqrt(2.0f * GravityCm * JumpHeightUnits * Scale);	// HL2: sqrt(2 * g * 21)
	Move->AirControl = 0.3f;
	Move->MaxAcceleration = WalkSpeedCm * 10.0f;				// sv_accelerate 10
	Move->BrakingDecelerationWalking = WalkSpeedCm * 6.0f;
	Move->GroundFriction = 8.0f;
	Move->bUseFlatBaseForFloorChecks = true;					// Source uses a box hull: stand on edges like HL2
	Move->PerchRadiusThreshold = 0.0f;
	Move->NavAgentProps.bCanCrouch = true;
}

void ALambdaCharacter::BeginPlay()
{
	Super::BeginPlay();

	const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();

	// Match Source gravity regardless of the project's DefaultGravityZ.
	if (UWorld* World = GetWorld())
	{
		const float WorldGravity = FMath::Abs(World->GetGravityZ());
		if (WorldGravity > KINDA_SMALL_NUMBER)
		{
			GetCharacterMovement()->GravityScale = (Settings.GravityUnits * Settings.UnitScale) / WorldGravity;
		}
	}

	FirstPersonCamera->PostProcessBlendWeight = 1.0f;
	FirstPersonCamera->PostProcessSettings.bOverride_AutoExposureBias = true;
	FirstPersonCamera->PostProcessSettings.AutoExposureBias = Settings.ExposureBias;
}

// ---------------------------------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------------------------------

void ALambdaCharacter::BuildInputAssets()
{
	if (MappingContext)
	{
		return;
	}

	MappingContext = NewObject<UInputMappingContext>(this, TEXT("IMC_Lambda"));

	auto MakeAction = [this](const TCHAR* Name, EInputActionValueType Type)
	{
		UInputAction* Action = NewObject<UInputAction>(this, Name);
		Action->ValueType = Type;
		return Action;
	};
	MoveAction = MakeAction(TEXT("IA_Move"), EInputActionValueType::Axis2D);
	LookAction = MakeAction(TEXT("IA_Look"), EInputActionValueType::Axis2D);
	JumpAction = MakeAction(TEXT("IA_Jump"), EInputActionValueType::Boolean);
	SprintAction = MakeAction(TEXT("IA_Sprint"), EInputActionValueType::Boolean);
	UseAction = MakeAction(TEXT("IA_Use"), EInputActionValueType::Boolean);
	QuitAction = MakeAction(TEXT("IA_Quit"), EInputActionValueType::Boolean);

	// Move: X = right (+) / left (-), Y = forward (+) / back (-)
	auto MapMoveKey = [this](const FKey& Key, bool bNegate, bool bSwizzleToY)
	{
		FEnhancedActionKeyMapping& Mapping = MappingContext->MapKey(MoveAction, Key);
		if (bNegate)
		{
			Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(MappingContext));
		}
		if (bSwizzleToY)
		{
			UInputModifierSwizzleAxis* Swizzle = NewObject<UInputModifierSwizzleAxis>(MappingContext);
			Swizzle->Order = EInputAxisSwizzle::YXZ;
			Mapping.Modifiers.Add(Swizzle);
		}
	};
	MapMoveKey(EKeys::W, false, true);
	MapMoveKey(EKeys::S, true, true);
	MapMoveKey(EKeys::D, false, false);
	MapMoveKey(EKeys::A, true, false);
	MapMoveKey(EKeys::Up, false, true);
	MapMoveKey(EKeys::Down, true, true);
	MapMoveKey(EKeys::Right, false, false);
	MapMoveKey(EKeys::Left, true, false);

	// Look: mouse. Y is negated so pushing the mouse forward looks up (same as the engine's first-person template).
	{
		FEnhancedActionKeyMapping& Mapping = MappingContext->MapKey(LookAction, EKeys::Mouse2D);
		UInputModifierNegate* Negate = NewObject<UInputModifierNegate>(MappingContext);
		Negate->bX = false;
		Negate->bY = true;
		Negate->bZ = false;
		Mapping.Modifiers.Add(Negate);
	}

	MappingContext->MapKey(JumpAction, EKeys::SpaceBar);
	MappingContext->MapKey(SprintAction, EKeys::LeftShift);
	MappingContext->MapKey(UseAction, EKeys::E);
	MappingContext->MapKey(QuitAction, EKeys::Escape);
}

void ALambdaCharacter::AddMappingContextToPlayer()
{
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC)
	{
		return;
	}
	ULocalPlayer* LocalPlayer = PC->GetLocalPlayer();
	if (!LocalPlayer)
	{
		return;
	}
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
	{
		Subsystem->AddMappingContext(MappingContext, 0);
	}
}

void ALambdaCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	BuildInputAssets();
	AddMappingContextToPlayer();

	UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!EIC)
	{
		UE_LOG(LogLambda, Error, TEXT("Enhanced Input component missing - check DefaultInputComponentClass in DefaultInput.ini"));
		return;
	}
	EIC->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ALambdaCharacter::Input_Move);
	EIC->BindAction(LookAction, ETriggerEvent::Triggered, this, &ALambdaCharacter::Input_Look);
	EIC->BindAction(JumpAction, ETriggerEvent::Started, this, &ALambdaCharacter::Input_JumpStart);
	EIC->BindAction(JumpAction, ETriggerEvent::Completed, this, &ALambdaCharacter::Input_JumpEnd);
	EIC->BindAction(SprintAction, ETriggerEvent::Started, this, &ALambdaCharacter::Input_SprintStart);
	EIC->BindAction(SprintAction, ETriggerEvent::Completed, this, &ALambdaCharacter::Input_SprintEnd);
	EIC->BindAction(UseAction, ETriggerEvent::Started, this, &ALambdaCharacter::Input_Use);
	EIC->BindAction(QuitAction, ETriggerEvent::Started, this, &ALambdaCharacter::Input_Quit);
}

void ALambdaCharacter::Input_Move(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	if (Controller)
	{
		AddMovementInput(GetActorForwardVector(), Axis.Y);
		AddMovementInput(GetActorRightVector(), Axis.X);
	}
}

void ALambdaCharacter::Input_Look(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	if (Controller)
	{
		AddControllerYawInput(Axis.X);
		AddControllerPitchInput(Axis.Y);
	}
}

void ALambdaCharacter::Input_JumpStart()
{
	Jump();
}

void ALambdaCharacter::Input_JumpEnd()
{
	StopJumping();
}

void ALambdaCharacter::Input_SprintStart()
{
	GetCharacterMovement()->MaxWalkSpeed = SprintSpeedCm;
}

void ALambdaCharacter::Input_SprintEnd()
{
	GetCharacterMovement()->MaxWalkSpeed = WalkSpeedCm;
}

AActor* ALambdaCharacter::FindUseEntity() const
{
	// CBasePlayer::FindUseEntity, primary trace only: a line from the eye along the view direction. The hit entity is
	// accepted when it is usable and within PLAYER_USE_RADIUS, measured with the vertical component clamped to the
	// player's own bounding box (so a tall door is judged by horizontal distance).
	const UWorld* World = GetWorld();
	if (!World || !Controller)
	{
		return nullptr;
	}

	const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();
	const float Scale = Settings.UnitScale;
	const float UseRadius = 100.0f * Scale;		// PLAYER_USE_RADIUS

	FVector EyeLocation;
	FRotator EyeRotation;
	GetActorEyesViewPoint(EyeLocation, EyeRotation);
	const FVector Forward = EyeRotation.Vector();

	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(LambdaPlayerUse), /*bTraceComplex=*/ true, this);
	if (!World->LineTraceSingleByChannel(Hit, EyeLocation, EyeLocation + Forward * 1024.0f * Scale, ECC_Visibility, Params))
	{
		return nullptr;
	}

	AActor* HitActor = Hit.GetActor();
	const ASourceBrushEntity* BrushEntity = Cast<ASourceBrushEntity>(HitActor);
	if (!BrushEntity || !BrushEntity->IsUsable())
	{
		return nullptr;
	}

	FVector Delta = Hit.ImpactPoint - EyeLocation;
	const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const float CenterZ = GetActorLocation().Z;
	Delta.Z = FMath::Max(0.0, FMath::Max(CenterZ - HalfHeight - Hit.ImpactPoint.Z, Hit.ImpactPoint.Z - (CenterZ + HalfHeight)));
	if (Delta.Size() >= UseRadius)
	{
		return nullptr;
	}
	return HitActor;
}

void ALambdaCharacter::Input_Use()
{
	if (AActor* UseTarget = FindUseEntity())
	{
		if (ASourceBrushEntity* BrushEntity = Cast<ASourceBrushEntity>(UseTarget))
		{
			BrushEntity->OnUsed(this);
		}
	}
}

void ALambdaCharacter::Input_Quit()
{
	UKismetSystemLibrary::QuitGame(this, Cast<APlayerController>(GetController()), EQuitPreference::Quit, false);
}
