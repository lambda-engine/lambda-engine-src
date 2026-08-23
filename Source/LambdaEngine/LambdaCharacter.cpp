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
#include "ProceduralMeshComponent.h"
#include "Engine/World.h"

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

	// Source draws the view model with its own narrower FOV (viewmodel_fov 54) so it does not distort at the edges.
	ViewModelMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("ViewModel"));
	ViewModelMesh->SetupAttachment(FirstPersonCamera);
	ViewModelMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ViewModelMesh->SetCastShadow(false);
	ViewModelMesh->bUseComplexAsSimpleCollision = false;
	ViewModelMesh->SetMobility(EComponentMobility::Movable);

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
		ViewModelMesh->ClearAllMeshSections();
		return true;
	}

	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	FSourceMDLFile Model;
	FString Error;
	if (!Model.Load(ModelPath, Scale, &Error))
	{
		UE_LOG(LogLambda, Warning, TEXT("View model '%s': %s"), *ModelPath, *Error);
		return false;
	}

	if (!ViewModelMaterials)
	{
		ViewModelMaterials = NewObject<ULambdaMaterialLibrary>(this);
		ViewModelMaterials->Initialize();
	}
	SourceGeometry::ApplyToComponent(ViewModelMesh, Model.Sections, ViewModelMaterials);

	// A view model's in-game position comes from its idle animation, which needs sequence decoding we do not have
	// yet - the .vvd holds only the reference pose, which for v_pistol sits about a metre above the eye. Until then,
	// centre the model on a configurable camera-relative offset so it reads as a held weapon.
	FBox Bounds(ForceInit);
	for (const FSourceMeshSection& Section : Model.Sections)
	{
		for (const FVector& V : Section.Vertices)
		{
			Bounds += V;
		}
	}

	const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();
	const FVector Centre = Bounds.IsValid ? Bounds.GetCenter() : FVector::ZeroVector;
	ViewModelMesh->SetRelativeScale3D(FVector(Settings.ViewModelScale));
	ViewModelMesh->SetRelativeRotation(Settings.ViewModelRotation);
	// The component rotates about its own origin, not about the mesh, so the centre-cancelling offset has to be
	// rotated too - otherwise any non-zero ViewModelRotation swings the model away from the camera.
	const FVector ScaledCentre = Settings.ViewModelRotation.RotateVector(Centre * Settings.ViewModelScale);
	ViewModelMesh->SetRelativeLocation(Settings.ViewModelOffset - ScaledCentre);

	UE_LOG(LogLambda, Log, TEXT("View model '%s': %d tris, bounds %s, placed at %s"),
		*ModelPath, Model.GetNumTriangles(), *Bounds.GetSize().ToString(), *ViewModelMesh->GetRelativeLocation().ToString());
	return true;
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
