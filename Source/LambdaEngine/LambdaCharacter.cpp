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
#include "LambdaWeapon.h"
#include "SourceAmmoDef.h"
#include "SourceMDLFile.h"
#include "SourceGeometryBuilder.h"
#include "LambdaMaterialLibrary.h"
#include "LambdaSourceSettings.h"
#include "SourceBSPWorldActor.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/Engine.h"
#include "SourceStudioModelComponent.h"
#include "UnrealClient.h"
#include "CollisionQueryParams.h"
#include "TimerManager.h"
#include "SourceImpactEffects.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/World.h"

// lambda.npc_create.auto <classname> spawns that NPC ahead of the player 2s after spawn, for scripted launches.
static FString GNPCCreateAuto;
static FAutoConsoleVariableRef CVarNPCCreateAuto(
	TEXT("lambda.npc_create.auto"),
	GNPCCreateAuto,
	TEXT("<classname>: npc_create it where the player looks, 2s after spawn"));

// lambda.decaltest.auto "<distance_cm> <angle_deg>" runs RunDecalTest shortly after spawn, so a scripted launch can
// frame decals identically every time (-ExecCmds runs before the pawn exists, hence the cvar + timer).
static FString GDecalTestAuto;
static FAutoConsoleVariableRef CVarDecalTestAuto(
	TEXT("lambda.decaltest.auto"),
	GDecalTestAuto,
	TEXT("\"<distance_cm> <angle_deg>\": stamp test impact decals and move to that viewpoint 2s after spawn"));

// lambda.fire.auto "<shots> [interval_s] [start_delay_s]" pulls the trigger from Tick, so a scripted launch can shoot
// without injecting input (which needs the game window in the foreground). A screenshot is taken after each shot.
static FString GFireAuto;
static FAutoConsoleVariableRef CVarFireAuto(
	TEXT("lambda.fire.auto"),
	GFireAuto,
	TEXT("\"<shots> [interval_s] [start_delay_s]\": fire the active weapon that many times from Tick, screenshot each"));

ALambdaCharacter::ALambdaCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

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
	// Source's viewmodel_fov: the view model is drawn at its own, narrower field of view.
	FirstPersonCamera->bEnableFirstPersonFieldOfView = true;
	FirstPersonCamera->bEnableFirstPersonScale = true;

	// Source draws the view model with its own narrower FOV (viewmodel_fov 54) so it does not distort at the edges.
	ViewModelMesh = CreateDefaultSubobject<USourceStudioModelComponent>(TEXT("ViewModel"));
	ViewModelMesh->SetupAttachment(FirstPersonCamera);
	ViewModelMesh->SetMobility(EComponentMobility::Movable);
	// Source draws the view model in its own pass with a compressed depth range so it can never intersect the
	// world; UE's first-person primitive path is the same idea, and it is what stops the gun pushing into a wall
	// the player stands against.
	ViewModelMesh->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::FirstPerson);

	MuzzleFlashMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("MuzzleFlash"));
	MuzzleFlashMesh->SetupAttachment(FirstPersonCamera);
	MuzzleFlashMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MuzzleFlashMesh->SetCastShadow(false);
	MuzzleFlashMesh->SetMobility(EComponentMobility::Movable);
	MuzzleFlashMesh->SetVisibility(false);
	// The flash belongs to the view model, so it has to live in the same first-person space as the gun.
	MuzzleFlashMesh->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::FirstPerson);

	MuzzleFlashLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("MuzzleFlashLight"));
	MuzzleFlashLight->SetupAttachment(FirstPersonCamera);
	MuzzleFlashLight->SetMobility(EComponentMobility::Movable);
	MuzzleFlashLight->SetVisibility(false);
	MuzzleFlashLight->SetCastShadows(false);
	// el->color in ProcessMuzzleFlashEvent: a warm orange flash.
	MuzzleFlashLight->SetLightColor(FLinearColor(FColor(255, 192, 64)));
	MuzzleFlashLight->SetIntensityUnits(ELightUnits::Candelas);
	// Channel 1 is "world only": the map geometry opts into it, the view model does not, so a flash bright enough
	// to light the room does not blow out the hands holding the gun a few centimetres away.
	MuzzleFlashLight->SetLightingChannels(false, true, false);

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

	// The POC map has no weapon or ammo entities, so arm the player directly. Once item_ammo_* / weapon_* entities
	// are implemented these come from the map instead.
	GiveWeapon(TEXT("weapon_pistol"));
	GiveAmmo(TEXT("Pistol"), 68);

	FirstPersonCamera->FirstPersonFieldOfView = Settings.ViewModelFOV;
	FirstPersonCamera->FirstPersonScale = Settings.ViewModelFirstPersonScale;

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
	AttackAction = MakeAction(TEXT("IA_Attack"), EInputActionValueType::Boolean);
	ReloadAction = MakeAction(TEXT("IA_Reload"), EInputActionValueType::Boolean);
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
	MappingContext->MapKey(AttackAction, EKeys::LeftMouseButton);
	MappingContext->MapKey(ReloadAction, EKeys::R);
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
	EIC->BindAction(AttackAction, ETriggerEvent::Started, this, &ALambdaCharacter::Input_AttackStart);
	EIC->BindAction(AttackAction, ETriggerEvent::Completed, this, &ALambdaCharacter::Input_AttackStop);
	EIC->BindAction(ReloadAction, ETriggerEvent::Started, this, &ALambdaCharacter::Input_ReloadStart);
	EIC->BindAction(ReloadAction, ETriggerEvent::Completed, this, &ALambdaCharacter::Input_ReloadStop);
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


// ---------------------------------------------------------------------------------------------------------------------
// Weapons and ammo
// ---------------------------------------------------------------------------------------------------------------------

void ALambdaCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// CBasePlayer::ItemPostFrame drives the active weapon every frame.
	if (ActiveWeapon)
	{
		ActiveWeapon->ItemPostFrame();
	}

	UpdateMuzzleFlash();

	// Scripted-launch aids (lambda.npc_create.auto / lambda.decaltest.auto): -ExecCmds applies its cvars *after*
	// BeginPlay, so they are read here, once, two seconds into play.
	if (!bAutoCommandsRun)
	{
		AutoCommandTimer += DeltaSeconds;
		if (AutoCommandTimer >= 2.0f)
		{
			bAutoCommandsRun = true;
			if (!GNPCCreateAuto.IsEmpty())
			{
				// "<classname> [distance_cm]"
				TArray<FString> Parts;
				GNPCCreateAuto.ParseIntoArrayWS(Parts);
				AutoFireTarget = NPCCreate(Parts[0], Parts.Num() > 1 ? FCString::Atof(*Parts[1]) : 5000.0f);
			}
			if (!GDecalTestAuto.IsEmpty())
			{
				TArray<FString> Parts;
				GDecalTestAuto.ParseIntoArrayWS(Parts);
				RunDecalTest(Parts.Num() > 0 ? FCString::Atof(*Parts[0]) : 90.0f, Parts.Num() > 1 ? FCString::Atof(*Parts[1]) : 45.0f,
					3, Parts.Num() > 2 ? Parts[2] : FString());
			}
			if (!GFireAuto.IsEmpty())
			{
				TArray<FString> Parts;
				GFireAuto.ParseIntoArrayWS(Parts);
				AutoFireShotsLeft = Parts.Num() > 0 ? FCString::Atoi(*Parts[0]) : 1;
				AutoFireInterval = Parts.Num() > 1 ? FCString::Atof(*Parts[1]) : 0.6f;
				AutoFireTimer = -(Parts.Num() > 2 ? FCString::Atof(*Parts[2]) : 2.0f);
			}
		}
	}

	if (DecalTestScreenshotTimer > 0.0f)
	{
		DecalTestScreenshotTimer -= DeltaSeconds;
		if (DecalTestScreenshotTimer <= 0.0f)
		{
			FScreenshotRequest::RequestScreenshot(TEXT("decaltest.png"), false, false);
		}
	}

	// lambda.fire.auto: a one-frame trigger pull per shot (the pistol is semi-automatic), screenshot shortly after.
	if (AutoFireShotsLeft > 0 || AutoFireShotTaken > 0)
	{
		AutoFireTimer += DeltaSeconds;
		if (ActiveWeapon && ActiveWeapon->bAttackHeld && AutoFirePulse)
		{
			ActiveWeapon->bAttackHeld = false;
			AutoFirePulse = false;
		}
		if (AutoFireShotsLeft > 0 && AutoFireTimer >= 0.0f)
		{
			// aim at whatever lambda.npc_create.auto spawned, so the scripted shots land on it
			if (AutoFireTarget.IsValid() && Controller)
			{
				FVector Eye;
				FRotator EyeRot;
				GetActorEyesViewPoint(Eye, EyeRot);
				Controller->SetControlRotation((AutoFireTarget->GetActorLocation() - Eye).Rotation());
			}
			if (ActiveWeapon)
			{
				ActiveWeapon->bAttackHeld = true;
				AutoFirePulse = true;
			}
			--AutoFireShotsLeft;
			++AutoFireShotTaken;
			AutoFireTimer = -AutoFireInterval;
			AutoFireScreenshotTimer = 0.15f;
		}
		if (AutoFireScreenshotTimer > 0.0f)
		{
			AutoFireScreenshotTimer -= DeltaSeconds;
			if (AutoFireScreenshotTimer <= 0.0f)
			{
				FScreenshotRequest::RequestScreenshot(FString::Printf(TEXT("fire_auto_%02d.png"), AutoFireShotTaken), false, false);
				if (AutoFireShotsLeft == 0)
				{
					// one more, a couple of seconds after the last shot, for what is left behind
					if (AutoFireShotTaken > 0 && AutoFireFinalTimer <= 0.0f)
					{
						AutoFireFinalTimer = 2.0f;
					}
				}
			}
		}
		if (AutoFireFinalTimer > 0.0f)
		{
			AutoFireFinalTimer -= DeltaSeconds;
			if (AutoFireFinalTimer <= 0.0f)
			{
				FScreenshotRequest::RequestScreenshot(TEXT("fire_auto_after.png"), false, false);
				AutoFireShotTaken = 0;
			}
		}
	}
}

void ALambdaCharacter::RunDecalTest(float DistanceCm, float AngleDeg, int32 Count, const FString& DecalName)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FVector EyeLocation;
	FRotator EyeRotation;
	GetActorEyesViewPoint(EyeLocation, EyeRotation);
	const FVector Forward = EyeRotation.Vector();
	const FVector Right = FRotationMatrix(EyeRotation).GetUnitAxis(EAxis::Y);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(LambdaDecalTest), /*bTraceComplex=*/ true, this);
	Params.bReturnFaceIndex = true;

	// A row of impacts across the wall ahead, 15 cm apart, through the same path a bullet takes.
	FHitResult Centre;
	bool bHaveCentre = false;
	for (int32 i = 0; i < Count; ++i)
	{
		const float Offset = (i - (Count - 1) * 0.5f) * 15.0f;
		const FVector Start = EyeLocation + Right * Offset;
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, Start, Start + Forward * 5000.0f, ECC_Visibility, Params))
		{
			if (DecalName.IsEmpty())
			{
				SourceImpact::PlayImpact(Hit, GetWorldMaterialLibrary(), this, Forward, 10.0f);
			}
			else
			{
				// a named decal material (e.g. "decals/hla/yblood/yblood1"), to look at one in isolation
				SourceImpact::SpawnDecal(Hit, GetWorldMaterialLibrary(), DecalName);
			}
			if (i == (Count - 1) / 2)
			{
				Centre = Hit;
				bHaveCentre = true;
			}
		}
	}
	if (!bHaveCentre)
	{
		UE_LOG(LogLambda, Warning, TEXT("decaltest: nothing ahead to stamp on"));
		return;
	}

	// Viewpoint: DistanceCm from the middle decal, swung AngleDeg off its normal around the wall's up axis.
	const FVector N = Centre.ImpactNormal;
	const FVector WallRight = FVector::CrossProduct(FVector::UpVector, N).GetSafeNormal();
	const float Rad = FMath::DegreesToRadians(AngleDeg);
	const FVector ViewDir = (N * FMath::Cos(Rad) + WallRight * FMath::Sin(Rad)).GetSafeNormal();
	const FVector CameraLocation = Centre.ImpactPoint + ViewDir * DistanceCm;

	// The camera sits above the capsule centre by its relative offset; place the actor so the camera lands there.
	const FVector CameraOffset = FirstPersonCamera ? FirstPersonCamera->GetRelativeLocation() : FVector::ZeroVector;
	SetActorLocation(CameraLocation - CameraOffset, false, nullptr, ETeleportType::TeleportPhysics);
	if (AController* C = GetController())
	{
		C->SetControlRotation((Centre.ImpactPoint - CameraLocation).Rotation());
	}
	UE_LOG(LogLambda, Log, TEXT("decaltest: %d decals at %s, viewing from %.0f cm at %.0f deg"),
		Count, *Centre.ImpactPoint.ToString(), DistanceCm, AngleDeg);
	DecalTestScreenshotTimer = 1.5f;	// Saved/Screenshots/.../decaltest.png once the view has settled
}

ALambdaWeapon* ALambdaCharacter::GiveWeapon(const FString& WeaponClassName)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	// Pick the class that implements this weapon; anything without a dedicated port still gets the shared behaviour.
	TSubclassOf<ALambdaWeapon> WeaponClass = ALambdaWeapon::StaticClass();
	if (WeaponClassName.Equals(TEXT("weapon_pistol"), ESearchCase::IgnoreCase))
	{
		WeaponClass = ALambdaWeaponPistol::StaticClass();
	}

	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.ObjectFlags |= RF_Transient;
	ALambdaWeapon* Weapon = World->SpawnActor<ALambdaWeapon>(WeaponClass, GetActorTransform(), Params);
	if (!Weapon)
	{
		return nullptr;
	}

	Weapon->SetOwningCharacter(this);
	Weapon->InitializeFromScript(WeaponClassName);
	Weapon->AttachToActor(this, FAttachmentTransformRules::SnapToTargetIncludingScale);

	if (ActiveWeapon)
	{
		ActiveWeapon->Destroy();
	}
	ActiveWeapon = Weapon;

	// Show the weapon's view model (models/weapons/v_pistol.mdl and friends, from the weapon script).
	SetViewModel(Weapon->GetWeaponInfo().ViewModel);

	return Weapon;
}


bool ALambdaCharacter::SetViewModel(const FString& ModelPath)
{
	if (!ViewModelMesh)
	{
		return false;
	}
	if (ModelPath.IsEmpty())
	{
		ViewModelMesh->ClearModel();
		return true;
	}

	if (!ViewModelMaterials)
	{
		ViewModelMaterials = NewObject<ULambdaMaterialLibrary>(this);
		ViewModelMaterials->Initialize();
	}
	if (!ViewModelMesh->SetModel(ModelPath, ViewModelMaterials))
	{
		return false;
	}

	// Source draws the view model as an entity standing at the player's eye with the player's view angles; the
	// model's own animation places the hands and weapon relative to that. So the component sits on the camera with
	// no offset of its own, and the settings below are only the equivalent of viewmodel_offset_x/y/z.
	const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();
	ViewModelMesh->SetRelativeScale3D(FVector(Settings.ViewModelScale));
	ViewModelMesh->SetRelativeRotation(Settings.ViewModelRotation);
	ViewModelMesh->SetRelativeLocation(Settings.ViewModelOffset);

	// CTempEnts::CacheMuzzleFlashes: build the flash materials with the weapon, not on its first shot.
	for (int32 i = 1; i <= 4; ++i)
	{
		ViewModelMaterials->GetSpriteMaterial(FString::Printf(TEXT("effects/muzzleflash%d_noz"), i));
	}

	// ACT_VM_DRAW is what Source plays when a weapon is deployed; it settles into the idle pose at its end.
	if (!SendViewModelAnim(TEXT("ACT_VM_DRAW")))
	{
		SendViewModelAnim(TEXT("ACT_VM_IDLE"));
	}

	const FSourceMDLFile* Model = ViewModelMesh->GetModel();
	UE_LOG(LogLambda, Log, TEXT("View model '%s': %d tris, %d bones, %d sequences, sequence %d (%.2fs)"),
		*ModelPath, Model ? Model->GetNumTriangles() : 0, Model ? Model->GetNumBones() : 0,
		Model ? Model->GetSequences().Num() : 0, ViewModelMesh->GetSequence(), ViewModelMesh->GetSequenceDuration());
	return true;
}

bool ALambdaCharacter::SendViewModelAnim(const FString& ActivityName)
{
	return ViewModelMesh && ViewModelMesh->PlayActivity(ActivityName);
}

int32 ALambdaCharacter::GetAmmoCount(const FString& AmmoType) const
{
	if (AmmoType.IsEmpty() || AmmoType.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		return 0;
	}
	const int32* Found = AmmoCounts.Find(AmmoType.ToLower());
	return Found ? *Found : 0;
}

int32 ALambdaCharacter::GiveAmmo(const FString& AmmoType, int32 Count)
{
	if (AmmoType.IsEmpty() || AmmoType.Equals(TEXT("None"), ESearchCase::IgnoreCase) || Count <= 0)
	{
		return 0;
	}

	// Clamp to the ammo type's carry limit (sk_max_<ammo> from skill.cfg).
	int32 MaxCarry = MAX_int32;
	if (const FSourceAmmoType* Type = FSourceAmmoDef::Get().Find(AmmoType))
	{
		if (Type->MaxCarry > 0.0f)
		{
			MaxCarry = FMath::RoundToInt(Type->MaxCarry);
		}
	}

	int32& Current = AmmoCounts.FindOrAdd(AmmoType.ToLower());
	const int32 Before = Current;
	Current = FMath::Min(Current + Count, MaxCarry);
	return Current - Before;
}

void ALambdaCharacter::RemoveAmmo(const FString& AmmoType, int32 Count)
{
	if (Count <= 0)
	{
		return;
	}
	if (int32* Current = AmmoCounts.Find(AmmoType.ToLower()))
	{
		*Current = FMath::Max(0, *Current - Count);
	}
}

void ALambdaCharacter::Input_AttackStart()
{
	if (ActiveWeapon)
	{
		ActiveWeapon->bAttackHeld = true;
		ActiveWeapon->bAttackPressedThisFrame = true;
	}
}

void ALambdaCharacter::Input_AttackStop()
{
	if (ActiveWeapon)
	{
		ActiveWeapon->bAttackHeld = false;
	}
}

void ALambdaCharacter::Input_ReloadStart()
{
	if (ActiveWeapon)
	{
		ActiveWeapon->bReloadHeld = true;
	}
}

void ALambdaCharacter::Input_ReloadStop()
{
	if (ActiveWeapon)
	{
		ActiveWeapon->bReloadHeld = false;
	}
}

void ALambdaCharacter::Input_Quit()
{
	UKismetSystemLibrary::QuitGame(this, Cast<APlayerController>(GetController()), EQuitPreference::Quit, false);
}

// ---------------------------------------------------------------------------------------------------------------------
// Muzzle flash - FX_MuzzleEffect (game/client/fx.cpp) and C_BaseAnimating::ProcessMuzzleFlashEvent
// ---------------------------------------------------------------------------------------------------------------------

// Debug aid: Source's first-person muzzle flash lives for 0.01s, which is a single frame and impossible to
// inspect. Setting this holds it on screen for that many seconds instead.
static float GMuzzleFlashHoldTime = 0.0f;
static FAutoConsoleVariableRef CVarMuzzleFlashHoldTime(
	TEXT("lambda.muzzleflash.holdtime"),
	GMuzzleFlashHoldTime,
	TEXT("Seconds to keep the muzzle flash sprite on screen (0 = Source's 0.01s)"));

void ALambdaCharacter::DoMuzzleFlash()
{
	if (!MuzzleFlashMesh || !ViewModelMesh || !ViewModelMesh->HasModel())
	{
		return;
	}

	// Source hangs the effect off the view model's "muzzle" attachment, which its animation moves with the gun.
	FVector MuzzleWorld, MuzzleForward;
	if (!ViewModelMesh->GetAttachmentWorld(TEXT("muzzle"), MuzzleWorld, MuzzleForward))
	{
		return;
	}

	const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();
	const float Scale = Settings.UnitScale;
	UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.0f;

	if (!ViewModelMaterials)
	{
		ViewModelMaterials = NewObject<ULambdaMaterialLibrary>(this);
		ViewModelMaterials->Initialize();
	}

	// The quads are built in the muzzle flash component's space, which is the camera's, so "facing the camera"
	// is simply "perpendicular to the component's forward axis".
	const FTransform ToLocal = MuzzleFlashMesh->GetComponentTransform().Inverse();
	const FVector LocalMuzzle = ToLocal.TransformPosition(MuzzleWorld);
	const FVector LocalForward = ToLocal.TransformVectorNoScale(MuzzleForward).GetSafeNormal();

	// CTempEnts::MuzzleFlash_Pistol_Player (c_te_legacytempents.cpp). Note this is a different effect from the
	// one NPCs and dropped weapons use: it is tighter, far shorter lived, and draws with the "_noz" materials so
	// it is not swallowed by the view model it sits on top of.
	static constexpr int32 NumFlashParticles = 5;
	static constexpr int32 NumFlashMaterials = 4;
	const float FlashScale = FMath::FRandRange(1.0f, 1.25f);
	const FLinearColor FlashTint = FLinearColor(FColor(255, 255, (uint8)FMath::RandRange(200, 255)));

	struct FFlashBucket
	{
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TArray<FLinearColor> Colors;
		TArray<FProcMeshTangent> Tangents;
	};
	FFlashBucket Buckets[NumFlashMaterials];

	for (int32 i = 1; i < NumFlashParticles + 1; ++i)
	{
		// Source picks one of the four flash materials per particle, so group the quads by material and emit one
		// mesh section per material rather than one draw per quad.
		FFlashBucket& Bucket = Buckets[FMath::RandRange(0, NumFlashMaterials - 1)];

		const FVector Centre = LocalMuzzle + LocalForward * (i * 4.0f * FlashScale * Scale);
		const float SizeUnits = FMath::FRandRange(6.0f, 8.0f) * (8 - i) / 6.0f * FlashScale;
		const float Half = SizeUnits * Scale * 0.5f;

		// Camera-facing quad with the random roll Source gives each particle.
		const float Roll = FMath::FRandRange(0.0f, 2.0f * PI);
		const FVector Right = FVector(0.0, FMath::Cos(Roll), FMath::Sin(Roll)) * Half;
		const FVector Up = FVector(0.0, -FMath::Sin(Roll), FMath::Cos(Roll)) * Half;

		const int32 Base = Bucket.Vertices.Num();
		Bucket.Vertices.Add(Centre - Right - Up);
		Bucket.Vertices.Add(Centre + Right - Up);
		Bucket.Vertices.Add(Centre + Right + Up);
		Bucket.Vertices.Add(Centre - Right + Up);
		Bucket.UVs.Add(FVector2D(0, 1)); Bucket.UVs.Add(FVector2D(1, 1));
		Bucket.UVs.Add(FVector2D(1, 0)); Bucket.UVs.Add(FVector2D(0, 0));
		for (int32 v = 0; v < 4; ++v)
		{
			Bucket.Normals.Add(FVector(-1, 0, 0));
			Bucket.Colors.Add(FLinearColor::White);
			Bucket.Tangents.Add(FProcMeshTangent(0, 1, 0));
		}
		Bucket.Triangles.Append({ Base, Base + 1, Base + 2, Base, Base + 2, Base + 3 });
	}

	MuzzleFlashMesh->ClearAllMeshSections();
	int32 SectionIndex = 0;
	for (int32 m = 0; m < NumFlashMaterials; ++m)
	{
		const FFlashBucket& Bucket = Buckets[m];
		if (Bucket.Vertices.Num() == 0)
		{
			continue;
		}
		// "_noz": the first-person flash materials set "$ignorez 1" so the flash is not swallowed by the very
		// view model it is attached to.
		UMaterialInterface* Material = ViewModelMaterials->GetSpriteMaterial(
			FString::Printf(TEXT("effects/muzzleflash%d_noz"), m + 1));
		if (!Material)
		{
			continue;
		}
		// m_uchColor: white with a slightly random warm-to-cool blue channel. Source varies this per particle
		// through vertex colour; one value for the whole flash is as far as a shared material instance goes.
		if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Material))
		{
			MID->SetVectorParameterValue(TEXT("Tint"), FlashTint);
			MID->SetScalarParameterValue(TEXT("Brightness"), Settings.MuzzleFlashBrightness);
		}
		MuzzleFlashMesh->CreateMeshSection_LinearColor(SectionIndex, Bucket.Vertices, Bucket.Triangles,
			Bucket.Normals, Bucket.UVs, Bucket.Colors, Bucket.Tangents, /*bCreateCollision=*/ false);
		MuzzleFlashMesh->SetMaterial(SectionIndex, Material);
		++SectionIndex;
	}
	if (SectionIndex == 0)
	{
		return;
	}

	MuzzleFlashMesh->SetVisibility(true);
	// m_flDieTime is 0.01s, well under a frame at any sane framerate, so hold the flash for one rendered frame
	// rather than letting it be skipped entirely.
	MuzzleFlashSpriteDieTime = Now + FMath::Max(0.01f, GMuzzleFlashHoldTime);
	MuzzleFlashHoldFrames = 1;

	// ProcessMuzzleFlashEvent's elight. Source's version only lights models, never the world, which is why a shot
	// in HL2 lights your hands but leaves the room dark; this one is sized to actually light the room, and is kept
	// off the view model by its lighting channel so it cannot blow out the hands a few centimetres away.
	if (MuzzleFlashLight && Settings.bMuzzleFlashLight)
	{
		// Only push a changed radius: SetAttenuationRadius writes the proxy directly (PushRadiusToRenderThread) and,
		// done in the same frame the light becomes visible, trips the "GPU Scene Lights is stale" ensure - a
		// multi-second stack walk on the first shot in development builds.
		const float NewRadius = Settings.MuzzleFlashLightRadiusUnits * Scale;
		if (NewRadius != MuzzleFlashLightRadius)
		{
			MuzzleFlashLightRadius = NewRadius;
			MuzzleFlashLight->SetAttenuationRadius(MuzzleFlashLightRadius);
		}
		MuzzleFlashLight->SetWorldLocation(MuzzleWorld);
		MuzzleFlashLight->SetIntensity(Settings.MuzzleFlashLightIntensity);
		if (MuzzleFlashLight->LightingChannels.bChannel0 != Settings.bMuzzleFlashLightsViewModel)
		{
			MuzzleFlashLight->SetLightingChannels(Settings.bMuzzleFlashLightsViewModel, true, false);
		}
		MuzzleFlashLight->SetVisibility(true);
		MuzzleFlashLightDieTime = Now + FMath::Max(0.05f, GMuzzleFlashHoldTime);	// el->die
	}
}

void ALambdaCharacter::UpdateMuzzleFlash()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const float Now = World->GetTimeSeconds();

	if (MuzzleFlashMesh && MuzzleFlashMesh->IsVisible())
	{
		if (MuzzleFlashHoldFrames > 0)
		{
			--MuzzleFlashHoldFrames;
		}
		else if (Now >= MuzzleFlashSpriteDieTime)
		{
			MuzzleFlashMesh->SetVisibility(false);
		}
	}

	if (MuzzleFlashLight && MuzzleFlashLight->IsVisible())
	{
		if (Now >= MuzzleFlashLightDieTime)
		{
			MuzzleFlashLight->SetVisibility(false);
		}
		// Source's elight decays its radius to zero over the 0.05 s (el->decay = radius / 0.05); over three frames
		// that is invisible, and pushing a new radius every frame trips UE's "GPU Scene Lights is stale" ensure
		// (a ~1 s stack walk on the first shot in development builds), so the light simply holds and goes out.
	}
}

float ALambdaCharacter::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	if (DamageAmount <= 0.0f || Health <= 0.0f)
	{
		return 0.0f;
	}
	// CBasePlayer::OnTakeDamage without the suit: the whole amount comes off health. Death is not handled yet -
	// health simply stops at zero.
	Health = FMath::Max(0.0f, Health - DamageAmount);
	UE_LOG(LogLambda, Log, TEXT("player took %.0f damage from %s, health %.0f"), DamageAmount, *GetNameSafe(DamageCauser), Health);
	return DamageAmount;
}

ULambdaMaterialLibrary* ALambdaCharacter::GetWorldMaterialLibrary() const
{
	if (WorldMaterialLibrary)
	{
		return WorldMaterialLibrary;
	}
	if (UWorld* World = GetWorld())
	{
		if (ASourceBSPWorldActor* BSPWorld = Cast<ASourceBSPWorldActor>(UGameplayStatics::GetActorOfClass(World, ASourceBSPWorldActor::StaticClass())))
		{
			WorldMaterialLibrary = BSPWorld->MaterialLibrary;
		}
	}
	return WorldMaterialLibrary;
}

AActor* ALambdaCharacter::NPCCreate(const FString& ClassName, float MaxDistanceCm)
{
	UWorld* World = GetWorld();
	ASourceBSPWorldActor* BSPWorld = World ? Cast<ASourceBSPWorldActor>(UGameplayStatics::GetActorOfClass(World, ASourceBSPWorldActor::StaticClass())) : nullptr;
	if (!BSPWorld)
	{
		UE_LOG(LogLambda, Warning, TEXT("npc_create: no BSP world actor"));
		return nullptr;
	}
	// CC_NPC_Create: trace from the eyes, spawn at the hit point, facing back at the player.
	FVector Eye;
	FRotator EyeRot;
	GetActorEyesViewPoint(Eye, EyeRot);
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(NPCCreate), true, this);
	FVector Spot = Eye + EyeRot.Vector() * MaxDistanceCm;
	if (World->LineTraceSingleByChannel(Hit, Eye, Eye + EyeRot.Vector() * MaxDistanceCm, ECC_Visibility, Params))
	{
		// Back off the surface by a hull's width so the NPC is not spawned into a wall...
		Spot = Hit.ImpactPoint + Hit.ImpactNormal * 40.0f;
	}
	// ...then UTIL_DropToFloor: the NPC stands on whatever is below that point.
	FHitResult Floor;
	if (World->LineTraceSingleByChannel(Floor, Spot + FVector(0, 0, 20.0f), Spot - FVector(0, 0, 5000.0f), ECC_Visibility, Params))
	{
		Spot = Floor.ImpactPoint + FVector(0, 0, 1.0f);
	}
	const float Yaw = (GetActorLocation() - Spot).Rotation().Yaw;
	AActor* NPC = BSPWorld->CreateNPC(ClassName, Spot, Yaw);
	UE_LOG(LogLambda, Display, TEXT("npc_create %s: %s"), *ClassName, NPC ? *NPC->GetActorLocation().ToString() : TEXT("failed"));
	return NPC;
}
