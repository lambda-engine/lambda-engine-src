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
#include "SourceStudioModelComponent.h"
#include "ProceduralMeshComponent.h"
#include "Components/PointLightComponent.h"
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
	ViewModelMesh = CreateDefaultSubobject<USourceStudioModelComponent>(TEXT("ViewModel"));
	ViewModelMesh->SetupAttachment(FirstPersonCamera);
	ViewModelMesh->SetMobility(EComponentMobility::Movable);

	MuzzleFlashMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("MuzzleFlash"));
	MuzzleFlashMesh->SetupAttachment(FirstPersonCamera);
	MuzzleFlashMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MuzzleFlashMesh->SetCastShadow(false);
	MuzzleFlashMesh->SetMobility(EComponentMobility::Movable);
	MuzzleFlashMesh->SetVisibility(false);

	MuzzleFlashLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("MuzzleFlashLight"));
	MuzzleFlashLight->SetupAttachment(FirstPersonCamera);
	MuzzleFlashLight->SetMobility(EComponentMobility::Movable);
	MuzzleFlashLight->SetVisibility(false);
	MuzzleFlashLight->SetCastShadows(false);
	// el->color in ProcessMuzzleFlashEvent: a warm orange flash.
	MuzzleFlashLight->SetLightColor(FLinearColor(FColor(255, 192, 64)));
	MuzzleFlashLight->SetIntensityUnits(ELightUnits::Candelas);

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

	UpdateMuzzleFlash();
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

	// FX_MuzzleEffect picks one of effects/muzzleflash1..4 per particle; one material for the whole burst is
	// close enough here and keeps this to a single mesh section.
	const FString SpriteName = FString::Printf(TEXT("effects/muzzleflash%d"), FMath::RandRange(1, 4));
	UMaterialInterface* SpriteMaterial = ViewModelMaterials->GetSpriteMaterial(SpriteName);
	if (!SpriteMaterial)
	{
		return;
	}

	// The quads are built in the muzzle flash component's space, which is the camera's, so "facing the camera"
	// is simply "perpendicular to the component's forward axis".
	const FTransform ToLocal = MuzzleFlashMesh->GetComponentTransform().Inverse();
	const FVector LocalMuzzle = ToLocal.TransformPosition(MuzzleWorld);
	const FVector LocalForward = ToLocal.TransformVectorNoScale(MuzzleForward).GetSafeNormal();

	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FLinearColor> Colors;
	TArray<FProcMeshTangent> Tangents;

	const float FlashScale = FMath::FRandRange(0.75f, 1.25f);

	// FX_MuzzleEffect lays 8 sprites along the muzzle direction, shrinking with distance.
	for (int32 i = 1; i < 9; ++i)
	{
		const FVector Centre = LocalMuzzle + LocalForward * (i * 2.0f * FlashScale * Scale);
		const float SizeUnits = (FMath::FRandRange(6.0f, 9.0f) * (12 - i) / 9.0f) * FlashScale;
		const float Half = SizeUnits * Scale * 0.5f;

		// Camera-facing quad with a random roll, as Source gives each particle.
		const float Roll = FMath::FRandRange(0.0f, 2.0f * PI);
		const FVector Right = FVector(0.0, FMath::Cos(Roll), FMath::Sin(Roll)) * Half;
		const FVector Up = FVector(0.0, -FMath::Sin(Roll), FMath::Cos(Roll)) * Half;

		const int32 Base = Vertices.Num();
		Vertices.Add(Centre - Right - Up);
		Vertices.Add(Centre + Right - Up);
		Vertices.Add(Centre + Right + Up);
		Vertices.Add(Centre - Right + Up);
		UVs.Add(FVector2D(0, 1)); UVs.Add(FVector2D(1, 1)); UVs.Add(FVector2D(1, 0)); UVs.Add(FVector2D(0, 0));
		for (int32 v = 0; v < 4; ++v)
		{
			Normals.Add(FVector(-1, 0, 0));
			Colors.Add(FLinearColor::White);
			Tangents.Add(FProcMeshTangent(0, 1, 0));
		}
		Triangles.Append({ Base, Base + 1, Base + 2, Base, Base + 2, Base + 3 });
	}

	MuzzleFlashMesh->ClearAllMeshSections();
	MuzzleFlashMesh->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, UVs, Colors, Tangents, /*bCreateCollision=*/ false);
	MuzzleFlashMesh->SetMaterial(0, SpriteMaterial);
	MuzzleFlashMesh->SetVisibility(true);
	MuzzleFlashSpriteDieTime = Now + 0.1f;		// pParticle->m_flDieTime

	// ProcessMuzzleFlashEvent's elight: a short warm flash lighting whatever the gun is pointed at.
	if (MuzzleFlashLight && Settings.bMuzzleFlashLight)
	{
		MuzzleFlashLightRadius = FMath::RandRange(32, 64) * Scale;
		MuzzleFlashLight->SetWorldLocation(MuzzleWorld);
		MuzzleFlashLight->SetAttenuationRadius(MuzzleFlashLightRadius);
		MuzzleFlashLight->SetIntensity(Settings.MuzzleFlashLightIntensity);
		MuzzleFlashLight->SetVisibility(true);
		MuzzleFlashLightDieTime = Now + 0.05f;	// el->die
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

	if (MuzzleFlashMesh && MuzzleFlashMesh->IsVisible() && Now >= MuzzleFlashSpriteDieTime)
	{
		MuzzleFlashMesh->SetVisibility(false);
	}

	if (MuzzleFlashLight && MuzzleFlashLight->IsVisible())
	{
		if (Now >= MuzzleFlashLightDieTime)
		{
			MuzzleFlashLight->SetVisibility(false);
		}
		else
		{
			// el->decay = radius / 0.05: the light shrinks away over its lifetime rather than popping off.
			const float Remaining = FMath::Max(0.0f, MuzzleFlashLightDieTime - Now) / 0.05f;
			MuzzleFlashLight->SetAttenuationRadius(FMath::Max(1.0f, MuzzleFlashLightRadius * Remaining));
		}
	}
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
