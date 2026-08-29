#include "LambdaCharacter.h"
#include "LambdaEngine.h"
#include "Core/LambdaSourceSettings.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "LambdaCharacterMovement.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Entities/SourceBrushEntity.h"
#include "Core/LambdaSourceSettings.h"
#include "Engine/HitResult.h"
#include "LambdaWeapon.h"
#include "Weapons/SourceAmmoDef.h"
#include "Core/SourceCoordinates.h"
#include "Entities/SourcePropPhysics.h"

// Defined next to GiveAmmo; BumpWeapon above it wants it too.
void ALambdaCharacterAddPickupHistory(TArray<ALambdaCharacter::FPickupEvent>& History, UWorld* World, const FString& Text);
#include "Weapons/SourceWeaponScript.h"
#include "Formats/SourceMDLFile.h"
#include "World/SourceGeometryBuilder.h"
#include "Materials/LambdaMaterialLibrary.h"
#include "Core/LambdaSourceSettings.h"
#include "World/SourceBSPWorldActor.h"
#include "EngineUtils.h"	// TActorIterator, for entfire.auto
#include "Kismet/GameplayStatics.h"
#include "Materials/SourceSurfaceProps.h"
#include "Audio/LambdaSoundLibrary.h"
#include "Audio/SourceSoundScript.h"
#include "Gameplay/SourceDamage.h"
#include "Engine/Engine.h"
#include "Rendering/SourceStudioModelComponent.h"
#include "Rendering/SourceRagdoll.h"
#include "Creatures/SourceNPCBase.h"
#include "UnrealClient.h"
#include "CollisionQueryParams.h"
#include "TimerManager.h"
#include "Rendering/SourceImpactEffects.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/World.h"

// npc_create.auto <classname> spawns that NPC ahead of the player 2s after spawn, for scripted launches.
static FString GNPCCreateAuto;
static FAutoConsoleVariableRef CVarNPCCreateAuto(
	TEXT("npc_create.auto"),
	GNPCCreateAuto,
	TEXT("<classname>: npc_create it where the player looks, 2s after spawn"));

// decaltest.auto "<distance_cm> <angle_deg>" runs RunDecalTest shortly after spawn, so a scripted launch can
// frame decals identically every time (-ExecCmds runs before the pawn exists, hence the cvar + timer).
static FString GDecalTestAuto;
static FAutoConsoleVariableRef CVarDecalTestAuto(
	TEXT("decaltest.auto"),
	GDecalTestAuto,
	TEXT("\"<distance_cm> <angle_deg>\": stamp test impact decals and move to that viewpoint 2s after spawn"));

// fire.auto "<shots> [interval_s] [start_delay_s]" pulls the trigger from Tick, so a scripted launch can shoot
// without injecting input (which needs the game window in the foreground). A screenshot is taken after each shot.
// setpos.auto "<x> <y> <z>" teleports the player to a Source-space position shortly after spawn, the way
// Source's setpos does, so a scripted run can start somewhere other than info_player_start.
static FString GSetPosAuto;
static FAutoConsoleVariableRef CVarSetPosAuto(
	TEXT("setpos.auto"),
	GSetPosAuto,
	TEXT("\"<x> <y> <z> [yaw] [pitch]\": move the player to that Source-space position 2s after spawn"));

// prop_create.auto "<model> [distance_cm]" drops a physics prop in front of the player after spawn.
static FString GPropCreateAuto;
static FAutoConsoleVariableRef CVarPropCreateAuto(
	TEXT("prop_create.auto"),
	GPropCreateAuto,
	TEXT("\"<model> [distance_cm]\": prop_physics_create where the player looks, 2s after spawn"));

// propcarry.auto "<grab_delay_s> [throw_delay_s]" exercises the +USE carry without injecting input.
// walk.auto "<seconds> [delay_s]" walks the player forward, for testing what he bumps into.
// pitchsweep.auto "<from> <to> <seconds> [delay_s]" sweeps the view up or down, Source pitch (+ is down).
static FString GPitchSweepAuto;
static FAutoConsoleVariableRef CVarPitchSweepAuto(
	TEXT("pitchsweep.auto"),
	GPitchSweepAuto,
	TEXT("\"<from> <to> <seconds> [delay_s]\": sweep the view pitch"));

// slot.auto "<bucket> [delay_s]" selects that weapon slot and confirms it, as the number keys would.
static FString GSlotAuto;
static FAutoConsoleVariableRef CVarSlotAuto(
	TEXT("slot.auto"),
	GSlotAuto,
	TEXT("\"<bucket> [delay_s]\": switch to the first weapon of that bucket"));

static FString GWalkAuto;
static FAutoConsoleVariableRef CVarWalkAuto(
	TEXT("walk.auto"),
	GWalkAuto,
	TEXT("\"<seconds> [delay_s]\": walk forward for that long"));

// hurt.auto "<amount> [type] [delay_s]" hurts the player on a timer, so the suit's reaction and the HUD's
// damage icons can be seen without anything pressing a key.
static FString GHurtAuto;
static FAutoConsoleVariableRef CVarHurtAuto(
	TEXT("hurt.auto"),
	GHurtAuto,
	TEXT("\"<amount> [damagetype] [delay_s]\": damage the player after that long"));

// thirdperson.auto "[delay_s]" switches the view a set number of seconds in, since the console commands
// themselves run before there is a player to switch.
static FString GThirdPersonAuto;
static FAutoConsoleVariableRef CVarThirdPersonAuto(
	TEXT("thirdperson.auto"),
	GThirdPersonAuto,
	TEXT("\"<delay_s> [return_s]\": third person after that long, and back to first at return_s"));

// use.auto "<delay_s> [second_delay_s]" presses +USE on whatever the player is looking at, once or twice, so a
// button and the thing it is wired to can be tested together without injecting a keypress.
static FString GUseAuto;
static FAutoConsoleVariableRef CVarUseAuto(
	TEXT("use.auto"),
	GUseAuto,
	TEXT("\"<delay_s> [second_delay_s]\": press +USE that long after play begins"));

// grab.auto "<delay_s>" takes a screenshot that long into play.
//
// Not "shot.auto": the engine has a SHOT exec command, and FParse::Command matches it as a PREFIX, so the
// engine swallowed the whole line, took a screenshot on frame one, and the cvar was never set - the same trap
// as +map being eaten by GetMapOverrideName. A name that is not another command's prefix is the fix. For comparing one lighting path
// against another: the two runs are identical but for a cvar, so the pictures can be measured against each
// other rather than looked at. A delay rather than immediately, because Lumen accumulates over frames and a
// picture taken on the first one shows it still converging.
static FString GShotAuto;
static FAutoConsoleVariableRef CVarShotAuto(
	TEXT("grab.auto"),
	GShotAuto,
	TEXT("\"<delay_s> [name]\": take a screenshot that long after play begins"));

// entfire.auto "<target> <input> [parameter] [delay_s]" fires one entity input a set number of seconds into
// play. ent_fire itself runs from -ExecCmds before the map's entities exist, so there is nothing to fire at.
static FString GEntFireAuto;
static FAutoConsoleVariableRef CVarEntFireAuto(
	TEXT("entfire.auto"),
	GEntFireAuto,
	TEXT("\"<target> <input> [parameter|-] [delay_s] | ...\": fire entity inputs once play begins"));

// playermodel.auto "<models/path.mdl>" wears a model from the moment play starts, so a rig can be looked at
// without anything typing cl_playermodel - the command itself runs before there is a player to dress.
static FString GPlayerModelAuto;
static FAutoConsoleVariableRef CVarPlayerModelAuto(
	TEXT("playermodel.auto"),
	GPlayerModelAuto,
	TEXT("\"<models/player/x.mdl>\": wear that model once play begins"));

static FString GSpeedLogAuto;
static FAutoConsoleVariableRef CVarSpeedLogAuto(
	TEXT("speedlog.auto"),
	GSpeedLogAuto,
	TEXT("\"<seconds>\": log the player's speed and height four times a second, for measuring surf and ladders"));

static FString GCrouchAuto;
static FAutoConsoleVariableRef CVarCrouchAuto(
	TEXT("crouch.auto"),
	GCrouchAuto,
	TEXT("\"<seconds> [delay_s]\": hold duck for that long"));

static FString GJumpAuto;
static FAutoConsoleVariableRef CVarJumpAuto(
	TEXT("jump.auto"),
	GJumpAuto,
	TEXT("\"<delay_s> [duck_after_s]\": jump, optionally duck that long after leaving the ground, and report "
		 "how high the feet got - which is how a plain jump and a crouch jump are measured"));

static FString GPropCarryAuto;
static FAutoConsoleVariableRef CVarPropCarryAuto(
	TEXT("propcarry.auto"),
	GPropCarryAuto,
	TEXT("\"<grab_delay_s> [throw_delay_s]\": pick up the prop ahead, then throw it"));

// firehold.auto "<seconds> [start_delay_s]" holds the trigger down, which is the only way to see what an
// automatic weapon does - fire.auto pulls and releases once per shot, so a machine gun never gets past its
// first round of recoil.
static FString GFireHoldAuto;
static FAutoConsoleVariableRef CVarFireHoldAuto(
	TEXT("firehold.auto"),
	GFireHoldAuto,
	TEXT("\"<seconds> [start_delay_s] [alt]\": hold the trigger down for that long; \"alt\" holds the second one"));

static FString GFireAuto;
static FAutoConsoleVariableRef CVarFireAuto(
	TEXT("fire.auto"),
	GFireAuto,
	TEXT("\"<shots> [interval_s] [start_delay_s]\": fire the active weapon that many times from Tick, screenshot each"));

/**
 * Whether the view model is drawn through UE's first-person primitive path - the one that keeps the gun from
 * pushing into a wall the player stands against, as Source's compressed depth range does. On; the switch is
 * kept because it was worth having while chasing an invisible view model.
 */
static bool GLambdaViewModelFirstPerson = true;
static FAutoConsoleVariableRef CVarLambdaViewModelFirstPerson(
	TEXT("viewmodel.firstperson"),
	GLambdaViewModelFirstPerson,
	TEXT("Draw the view model through UE's first-person primitive path"));

ALambdaCharacter::ALambdaCharacter(const FObjectInitializer& ObjectInitializer)
	// Quake's movement in place of Unreal's: see ULambdaCharacterMovement.
	: Super(ObjectInitializer.SetDefaultSubobjectClass<ULambdaCharacterMovement>(ACharacter::CharacterMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = true;

	const ULambdaSourceSettings* Settings = GetDefault<ULambdaSourceSettings>();
	const float Scale = Settings ? Settings->UnitScale : 1.905f;
	const float RadiusCm = (Settings ? Settings->PlayerCapsuleRadiusUnits : 16.0f) * Scale;
	const float HalfHeightCm = (Settings ? Settings->PlayerCapsuleHalfHeightUnits : 36.0f) * Scale;
	const float EyeHeightCm = (Settings ? Settings->PlayerEyeHeightUnits : 64.0f) * Scale;

	GetCapsuleComponent()->InitCapsuleSize(RadiusCm, HalfHeightCm);

	// Unreal's own physics interaction shoves objects with a force of its own choosing; Source's player is a
	// physics shadow that pushes what it walks into at its own walking speed, and no harder (PushPhysicsObject).
	GetCharacterMovement()->bEnablePhysicsInteraction = false;
	BaseEyeHeight = EyeHeightCm - HalfHeightCm;

	// VEC_DUCK_HULL / VEC_DUCK_VIEW: the ducked player is 36 units tall against 72 standing, and sees from 28
	// units up rather than 64. Both are stated from the feet, so they lose the half height to become Unreal's,
	// which measures from the middle of the capsule.
	const float DuckHalfHeightCm = 18.0f * Scale;		// a 36 unit hull
	GetCharacterMovement()->SetCrouchedHalfHeight(DuckHalfHeightCm);
	CrouchedEyeHeight = 28.0f * Scale - DuckHalfHeightCm;

	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = true;
	bUseControllerRotationRoll = false;

	FirstPersonCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
	FirstPersonCamera->SetupAttachment(GetCapsuleComponent());
	FirstPersonCamera->SetRelativeLocation(FVector(0.0f, 0.0f, EyeHeightCm - HalfHeightCm));
	FirstPersonCamera->bUsePawnControlRotation = false;	// set from the control rotation plus the punch angle each tick
	FirstPersonCamera->FieldOfView = 90.0f;
	// Source's viewmodel_fov: the view model is drawn at its own, narrower field of view.
	FirstPersonCamera->bEnableFirstPersonFieldOfView = true;
	FirstPersonCamera->bEnableFirstPersonScale = true;

	// Source draws the view model with its own narrower FOV (viewmodel_fov 54) so it does not distort at the edges.
	ViewModelMesh = CreateDefaultSubobject<USourceStudioModelComponent>(TEXT("ViewModel"));
	ViewModelMesh->SetupAttachment(FirstPersonCamera);
	ViewModelMesh->SetMobility(EComponentMobility::Movable);

	// The player's own body: legs the owner sees, and a whole body only its shadow gets out of. Both hang off
	// the capsule rather than the camera - they belong to the pawn's position, not to where it is looking.
	LegsMesh = CreateDefaultSubobject<USourceStudioModelComponent>(TEXT("PlayerLegs"));
	LegsMesh->SetupAttachment(GetCapsuleComponent());
	LegsMesh->SetMobility(EComponentMobility::Movable);
	LegsMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	BodyMesh = CreateDefaultSubobject<USourceStudioModelComponent>(TEXT("PlayerBody"));
	BodyMesh->SetupAttachment(GetCapsuleComponent());
	BodyMesh->SetMobility(EComponentMobility::Movable);
	BodyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// The weapon the shadow holds. Not attached to a bone socket - the pose is composed on the CPU each frame,
	// so UpdatePlayerBody places it at the hand bone by hand.
	WeaponShadowMesh = CreateDefaultSubobject<USourceStudioModelComponent>(TEXT("WeaponShadow"));
	WeaponShadowMesh->SetupAttachment(GetCapsuleComponent());
	WeaponShadowMesh->SetMobility(EComponentMobility::Movable);
	WeaponShadowMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// Source draws the view model in its own pass with a compressed depth range so it can never intersect the
	// world; UE's first-person primitive path is the same idea, and it is what stops the gun pushing into a wall
	// the player stands against.


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
	// Movement is Quake's now (ULambdaCharacterMovement), so the numbers Unreal would use to shape acceleration
	// and braking no longer apply: sv_accelerate, sv_friction and sv_stopspeed do that instead. MaxAcceleration
	// survives only as the scale the input vector is read back out of, so it wants to be exactly the top speed.
	Move->MaxAcceleration = WalkSpeedCm;
	Move->BrakingDecelerationWalking = 0.0f;
	Move->GroundFriction = 0.0f;
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
	GiveWeapon(TEXT("weapon_crowbar"));
	GiveWeapon(TEXT("weapon_pistol"));
	GiveWeapon(TEXT("weapon_smg1"));
	GiveWeapon(TEXT("weapon_shotgun"));
	GiveAmmo(TEXT("Pistol"), 68);
	GiveAmmo(TEXT("SMG1"), 135);
	GiveAmmo(TEXT("Buckshot"), 30);

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
	CrouchAction = MakeAction(TEXT("IA_Crouch"), EInputActionValueType::Boolean);
	UseAction = MakeAction(TEXT("IA_Use"), EInputActionValueType::Boolean);
	AttackAction = MakeAction(TEXT("IA_Attack"), EInputActionValueType::Boolean);
	Attack2Action = MakeAction(TEXT("IA_Attack2"), EInputActionValueType::Boolean);
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
	MappingContext->MapKey(CrouchAction, EKeys::LeftControl);
	MappingContext->MapKey(UseAction, EKeys::E);
	MappingContext->MapKey(AttackAction, EKeys::LeftMouseButton);
	MappingContext->MapKey(Attack2Action, EKeys::RightMouseButton);
	MappingContext->MapKey(ReloadAction, EKeys::R);
	// Escape is not bound here: it belongs to the pause menu, which the viewport client opens with it.
	// Quitting is what the menu's QUIT does. The action is left in place for anyone who wants to bind one.
	static const FKey SlotKeys[5] = { EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five };
	for (int32 i = 0; i < 5; ++i)
	{
		SlotActions[i] = NewObject<UInputAction>(this);
		MappingContext->MapKey(SlotActions[i], SlotKeys[i]);
	}
	LastInvAction = NewObject<UInputAction>(this);
	MappingContext->MapKey(LastInvAction, EKeys::Q);
	InvNextAction = NewObject<UInputAction>(this);
	MappingContext->MapKey(InvNextAction, EKeys::MouseScrollDown);
	InvPrevAction = NewObject<UInputAction>(this);
	MappingContext->MapKey(InvPrevAction, EKeys::MouseScrollUp);
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
	EIC->BindAction(CrouchAction, ETriggerEvent::Started, this, &ALambdaCharacter::Input_CrouchStart);
	EIC->BindAction(CrouchAction, ETriggerEvent::Completed, this, &ALambdaCharacter::Input_CrouchEnd);
	EIC->BindAction(UseAction, ETriggerEvent::Started, this, &ALambdaCharacter::Input_Use);
	EIC->BindAction(AttackAction, ETriggerEvent::Started, this, &ALambdaCharacter::Input_AttackStart);
	EIC->BindAction(Attack2Action, ETriggerEvent::Started, this, &ALambdaCharacter::Input_Attack2Start);
	EIC->BindAction(SlotActions[0], ETriggerEvent::Started, this, &ALambdaCharacter::Input_Slot1);
	EIC->BindAction(SlotActions[1], ETriggerEvent::Started, this, &ALambdaCharacter::Input_Slot2);
	EIC->BindAction(SlotActions[2], ETriggerEvent::Started, this, &ALambdaCharacter::Input_Slot3);
	EIC->BindAction(SlotActions[3], ETriggerEvent::Started, this, &ALambdaCharacter::Input_Slot4);
	EIC->BindAction(SlotActions[4], ETriggerEvent::Started, this, &ALambdaCharacter::Input_Slot5);
	EIC->BindAction(LastInvAction, ETriggerEvent::Started, this, &ALambdaCharacter::Input_LastInv);
	EIC->BindAction(InvNextAction, ETriggerEvent::Started, this, &ALambdaCharacter::Input_InvNext);
	EIC->BindAction(InvPrevAction, ETriggerEvent::Started, this, &ALambdaCharacter::Input_InvPrev);
	EIC->BindAction(AttackAction, ETriggerEvent::Completed, this, &ALambdaCharacter::Input_AttackStop);
	EIC->BindAction(Attack2Action, ETriggerEvent::Completed, this, &ALambdaCharacter::Input_Attack2Stop);
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
	// EnableSprint( false ): no running about with a crate in your hands.
	if (CarriedProp.IsValid())
	{
		return;
	}

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
	// CBasePlayer::PlayerUse: a carried object is put down first, then whatever is in front is used - a button,
	// a door, or a physics prop light enough to pick up.
	if (CarriedProp.IsValid())
	{
		DropCarriedProp(false);
		return;
	}

	if (AActor* UseTarget = FindUseEntity())
	{
		if (ASourceBrushEntity* BrushEntity = Cast<ASourceBrushEntity>(UseTarget))
		{
			BrushEntity->OnUsed(this);
			return;
		}
	}
	TogglePropCarry();
}

void ALambdaCharacter::TogglePropCarry()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;

	// CBasePlayer::FindUseEntity looks along the player's view; PLAYER_USE_RADIUS is 80 units.
	FVector Eye;
	FRotator EyeRot;
	GetActorEyesViewPoint(Eye, EyeRot);
	const FVector Forward = EyeRot.Vector();

	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(LambdaUsePickup), true, this);
	if (!World->LineTraceSingleByChannel(Hit, Eye, Eye + Forward * 80.0f * Scale, ECC_Visibility, Params))
	{
		UE_LOG(LogLambda, Verbose, TEXT("+use: nothing within reach"));
		return;
	}
	UE_LOG(LogLambda, Verbose, TEXT("+use: hit %s at %s"), *GetNameSafe(Hit.GetActor()), *Hit.ImpactPoint.ToString());
	ASourcePropPhysics* Prop = Cast<ASourcePropPhysics>(Hit.GetActor());
	if (!Prop || Prop->IsHeld() || Prop->IsPickupPrevented())
	{
		return;
	}

	// CHL2_Player::PlayerUse -> CBasePlayer::CanPickupObject( pObject, 35, 128 ): too heavy or too big to hold.
	constexpr float PlayerPickupMassLimitKg = 35.0f;
	constexpr float PlayerPickupSizeLimitUnits = 128.0f;
	if (Prop->GetMass() > PlayerPickupMassLimitKg || Prop->GetSizeUnits() > PlayerPickupSizeLimitUnits)
	{
		UE_LOG(LogLambda, Verbose, TEXT("cannot pick up %s: %.1f kg, %.0f units"),
			*GetNameSafe(Prop), Prop->GetMass(), Prop->GetSizeUnits());
		return;
	}

	Prop->StartCarry(this);
	CarriedProp = Prop;
	// The prop keeps the orientation it had, relative to where the player is looking, as Source's grab controller
	// does when it stores the object's angles at pickup.
	CarriedPropRelativeRotation = (Prop->GetActorQuat() * FQuat(FRotator(0.0f, EyeRot.Yaw, 0.0f)).Inverse()).Rotator();

	// CPlayerPickupController::Init holsters the weapon and takes sprint away while you have your hands full.
	if (ActiveWeapon)
	{
		ActiveWeapon->Holster();
	}
}

float ALambdaCharacter::PlayerHullRadiusCm() const
{
	// CollisionProp()->OBBMaxs().Length2D(): the player's box is as wide as the capsule is across, so its 2D
	// diagonal is what a carried object has to clear.
	const float HalfWidth = GetCapsuleComponent()->GetScaledCapsuleRadius();
	return HalfWidth * UE_SQRT_2;
}

void ALambdaCharacter::UpdatePropCarry(float DeltaSeconds)
{
	// The +USE carry, rebuilt from one rule: the prop is held at the farthest point along the view ray where it
	// actually fits, out to arm's length. One prop-shaped sweep finds that point - there is no pitch clamp, no
	// pull-in shortcut and no radial push-out, each of which was a way for the hold point to end up somewhere
	// the prop could not be, with physics squeezing it up past the player's head as the answer. Looking straight
	// down simply parks the prop at your feet, under the crosshair.
	ASourcePropPhysics* Prop = CarriedProp.Get();
	if (!Prop)
	{
		// The prop was destroyed while it was being held - shot to pieces, or it left the world. Let go properly
		// so the weapon comes back out.
		if (!CarriedProp.IsExplicitlyNull())
		{
			DropCarriedProp(false);
		}
		return;
	}
	UWorld* World = GetWorld();
	// "pPlayer->GetGroundEntity() == pEntity": standing on the thing you are holding drops it. A prop taken out
	// of the player's hands by something stronger (the barnacle) is let go of the same way.
	const FHitResult& Floor = GetCharacterMovement()->CurrentFloor.HitResult;
	if (!World || Floor.GetActor() == Prop || Prop->IsCarryRevoked())
	{
		DropCarriedProp(false);
		return;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;

	FVector Eye;
	FRotator EyeRot;
	GetActorEyesViewPoint(Eye, EyeRot);
	const FVector Forward = EyeRot.Vector();

	// How much room the prop needs between its centre and the player, and how far out it is preferred:
	// CGrabController's 24 units past the clearance radius.
	const float RadiusCm = PlayerHullRadiusCm() + Prop->GetExtentAlong(-Forward);
	const float PreferredCm = 24.0f * Scale + RadiusCm * 2.0f - RadiusCm;

	// Sweep the prop's own shape from the eye along the view. Where it stops is where the prop can actually be:
	// against a wall it comes back toward the player, over open floor it rides the ray, looking down it settles
	// where the box meets the ground.
	float ReachCm = PreferredCm;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(LambdaCarryHold), false, this);
	Params.AddIgnoredActor(Prop);
	const FVector SweepExtent = Prop->GetHullExtent() * 0.9f;
	FHitResult Block;
	if (SweepExtent.GetMin() > 0.0f && World->SweepSingleByObjectType(Block, Eye, Eye + Forward * PreferredCm,
		Prop->GetActorQuat(), FCollisionObjectQueryParams(ECC_WorldStatic),
		FCollisionShape::MakeBox(FVector3f(SweepExtent)), Params))
	{
		if (Block.bStartPenetrating)
		{
			// The box already overlaps the world at the eye (back to a wall): a line stands in, less the room the
			// prop needs in front of it.
			FHitResult Line;
			if (World->LineTraceSingleByObjectType(Line, Eye, Eye + Forward * PreferredCm,
				FCollisionObjectQueryParams(ECC_WorldStatic), Params))
			{
				ReachCm = Line.Distance - Prop->GetExtentAlong(Forward);
			}
		}
		else
		{
			ReachCm = (Block.Location - Eye).Size();
		}
	}

	// No room to hold the prop even at its clearance radius: it cannot be carried here (the wedged case Source's
	// error check caught).
	if (ReachCm < RadiusCm)
	{
		UE_LOG(LogLambda, Verbose, TEXT("carry: no room to hold (%.1f < %.1f units), dropping"),
			ReachCm / Scale, RadiusCm / Scale);
		DropCarriedProp(false);
		return;
	}

	const FVector Target = Eye + Forward * FMath::Min(ReachCm, PreferredCm);

	// How the prop actually reads on screen: how far it is from the eye, and how far above the crosshair it sits.
	const FVector ToProp = Prop->GetActorLocation() - Eye;
	const float AboveCrosshair = FMath::RadiansToDegrees(
		FMath::Asin(FMath::Clamp(ToProp.GetSafeNormal().Z, -1.0f, 1.0f)) - FMath::Asin(FMath::Clamp(Forward.Z, -1.0f, 1.0f)));
	UE_LOG(LogLambda, VeryVerbose, TEXT("carry: pitch %.1f reach %.1f | prop %.1f units away, %+.1f deg above crosshair, target %.1f units off"),
		EyeRot.Pitch, ReachCm / Scale, ToProp.Size() / Scale, AboveCrosshair, (Target - Prop->GetActorLocation()).Size() / Scale);

	// The prop keeps the yaw it was picked up at, relative to where the player is looking.
	const FRotator TargetRotation = (FQuat(CarriedPropRelativeRotation) * FQuat(FRotator(0.0f, EyeRot.Yaw, 0.0f))).Rotator();
	if (!Prop->UpdateCarry(Target, TargetRotation, DeltaSeconds, 12.0f))
	{
		DropCarriedProp(false);
	}
}


void ALambdaCharacter::NotifyHit(UPrimitiveComponent* MyComp, AActor* Other, UPrimitiveComponent* OtherComp, bool bSelfMoved,
	FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit)
{
	Super::NotifyHit(MyComp, Other, OtherComp, bSelfMoved, HitLocation, HitNormal, NormalImpulse, Hit);
	PushPhysicsObject(Hit);
}

void ALambdaCharacter::PushPhysicsObject(const FHitResult& Hit)
{
	// The prop in the player's hands is driven by the grab controller; walking into it must not shove it as well.
	if (CarriedProp.IsValid() && Hit.GetActor() == CarriedProp.Get())
	{
		return;
	}
	ASourcePropPhysics::ShadowPush(Hit.GetComponent(), Hit, GetVelocity(), TEXT("player"));
}

void ALambdaCharacter::DropCarriedProp(bool bThrown)
{
	if (ASourcePropPhysics* Prop = CarriedProp.Get())
	{
		Prop->StopCarry(bThrown);
	}
	CarriedProp.Reset();
	// Shutdown(): the weapon comes back out and the player can sprint again.
	if (ActiveWeapon)
	{
		ActiveWeapon->Deploy();
	}
}

bool ALambdaCharacter::ThrowCarriedProp()
{
	ASourcePropPhysics* Prop = CarriedProp.Get();
	if (!Prop)
	{
		return false;
	}
	UPrimitiveComponent* Body = Prop->GetPhysicsBody();
	// Shutdown( true ) first: the prop is back to its real mass before the throw is applied to it.
	DropCarriedProp(true);
	if (!Body)
	{
		return true;
	}

	// CPlayerPickupController::Use, IN_ATTACK: player_throwforce 1000, scaled by the object's mass, plus a bit of
	// spin. Source's force is in its own units; converted here to the impulse UE wants.
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const float MassFactor = FMath::GetMappedRangeValueClamped(FVector2D(0.5f, 15.0f), FVector2D(0.5f, 4.0f),
		FMath::Clamp(Body->GetMass(), 0.5f, 15.0f));
	FVector Eye;
	FRotator EyeRot;
	GetActorEyesViewPoint(Eye, EyeRot);
	Body->AddImpulse(EyeRot.Vector() * 1000.0f * MassFactor * Scale);
	Body->AddAngularImpulseInDegrees(FVector(FMath::FRandRange(-10.0f, 10.0f), FMath::FRandRange(-10.0f, 10.0f),
		FMath::FRandRange(-10.0f, 10.0f)) * MassFactor * Scale);
	return true;
}


// ---------------------------------------------------------------------------------------------------------------------
// Weapons and ammo
// ---------------------------------------------------------------------------------------------------------------------

void ALambdaCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UpdateEyeHeight(DeltaSeconds);
	UpdateStepSound(DeltaSeconds);
	UpdatePlayerBody(DeltaSeconds);

	// CheckSuitUpdate: the suit works through whatever it has been given to say.
	SuitVoice.Tick(this, GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f);

	// CBasePlayer::ItemPostFrame drives the active weapon every frame.
	if (ActiveWeapon)
	{
		ActiveWeapon->ItemPostFrame();
	}

	// pitchsweep.auto: drag the view up or down at a steady rate, as a player would with the mouse.
	if (SweepSeconds > 0.0f)
	{
		if (SweepDelay > 0.0f)
		{
			SweepDelay -= DeltaSeconds;
		}
		else if (AController* PC = GetController())
		{
			SweepElapsed = FMath::Min(SweepElapsed + DeltaSeconds, SweepSeconds);
			FRotator View = PC->GetControlRotation();
			View.Pitch = FMath::Lerp(SweepFromPitch, SweepToPitch, SweepElapsed / SweepSeconds);
			PC->SetControlRotation(View);
		}
	}

	// The scripted view is enforced for a moment (see setpos.auto), then the deferred spawn commands fire into it.
	if (AutoViewHoldSeconds > 0.0f)
	{
		AutoViewHoldSeconds -= DeltaSeconds;
		if (AController* PC = GetController())
		{
			PC->SetControlRotation(AutoViewRotation);
		}
	}
	if (AutoSpawnDelay > 0.0f)
	{
		AutoSpawnDelay -= DeltaSeconds;
		if (AutoSpawnDelay <= 0.0f)
		{
			if (!PendingNPCCreate.IsEmpty())
			{
				NPCCreate(PendingNPCCreate);
				PendingNPCCreate.Reset();
			}
			if (!PendingPropCreate.IsEmpty())
			{
				TArray<FString> Parts;
				PendingPropCreate.ParseIntoArrayWS(Parts);
				if (Parts.Num() > 0)
				{
					PropCreate(Parts[0], Parts.Num() > 1 ? FCString::Atof(*Parts[1]) : 300.0f);
				}
				PendingPropCreate.Reset();
			}
		}
	}

	// thirdperson.auto: step outside the player's eyes on a timer, and optionally back in again - the return
	// trip is where a view mode's restoring goes wrong, so it wants testing as much as the leaving does.
	if (AutoThirdPersonDelay > 0.0f)
	{
		AutoThirdPersonDelay -= DeltaSeconds;
		if (AutoThirdPersonDelay <= 0.0f)
		{
			SetThirdPerson(true);
		}
	}
	if (AutoFirstPersonDelay > 0.0f)
	{
		AutoFirstPersonDelay -= DeltaSeconds;
		if (AutoFirstPersonDelay <= 0.0f)
		{
			SetThirdPerson(false);
		}
	}

	// hurt.auto: damage on a timer, so the suit's reaction and the damage icons can be watched.
	if (AutoHurtDelay > 0.0f)
	{
		AutoHurtDelay -= DeltaSeconds;
		if (AutoHurtDelay <= 0.0f)
		{
			FHitResult Hit;
			Hit.ImpactPoint = GetActorLocation();
			Hit.Location = Hit.ImpactPoint;
			FSourceDamageEvent Info(AutoHurtAmount, Hit, -GetActorForwardVector(), UDamageType::StaticClass(),
				FVector::ZeroVector, SourceDamage::TypeFromName(AutoHurtType));
			TakeDamage(AutoHurtAmount, Info, nullptr, nullptr);
			UE_LOG(LogLambda, Display, TEXT("hurt.auto %.0f %s: health %.0f, armour %.0f"),
				AutoHurtAmount, *AutoHurtType, Health, Armor);
		}
	}

	// slot.auto: a scripted press of the slot key plus the confirming attack.
	if (AutoSlotDelay > 0.0f)
	{
		AutoSlotDelay -= DeltaSeconds;
		if (AutoSlotDelay <= 0.0f)
		{
			SelectSlot(AutoSlotBucket);
			ConfirmWeaponSelection();
			UE_LOG(LogLambda, Display, TEXT("slot.auto %d -> %s"), AutoSlotBucket,
				ActiveWeapon ? *ActiveWeapon->GetWeaponClassName() : TEXT("none"));
		}
	}

	// jump.auto: jump, optionally duck in mid air, and report how high the feet reached.
	if (bAutoJumpArmed)
	{
		const float Scale = ULambdaSourceSettings::Get().UnitScale;
		const float FeetZ = GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		if (AutoJumpDelay > 0.0f)
		{
			AutoJumpDelay -= DeltaSeconds;
			AutoJumpStartFeetZ = FeetZ;
			AutoJumpPeakFeetZ = FeetZ;
			AutoJumpPeakAirSpeed = 0.0f;
			if (AutoJumpDelay <= 0.0f)
			{
				Jump();
				AutoJumpElapsed = 0.0f;
				AutoJumpSpeedAtLaunch = GetCharacterMovement() ? GetCharacterMovement()->Velocity.Size2D() : 0.0f;
			}
		}
		else
		{
			AutoJumpElapsed += DeltaSeconds;
			AutoJumpPeakFeetZ = FMath::Max(AutoJumpPeakFeetZ, FeetZ);
			if (GetCharacterMovement() && GetCharacterMovement()->IsFalling())
			{
				AutoJumpPeakAirSpeed = FMath::Max(AutoJumpPeakAirSpeed, GetCharacterMovement()->Velocity.Size2D());
			}
			if (AutoJumpDuckAfter >= 0.0f && AutoJumpElapsed >= AutoJumpDuckAfter && !bIsCrouched)
			{
				Crouch();
			}
			// Back on the ground, so the jump is over and worth reporting.
			if (AutoJumpElapsed > 0.2f && GetCharacterMovement() && GetCharacterMovement()->IsMovingOnGround())
			{
				bAutoJumpArmed = false;
				UE_LOG(LogLambda, Display, TEXT("jump.auto: feet reached %.1f units above where they started, %.0f u/s at launch, fastest in the air %.0f u/s%s"),
					(AutoJumpPeakFeetZ - AutoJumpStartFeetZ) / Scale, AutoJumpSpeedAtLaunch / Scale, AutoJumpPeakAirSpeed / Scale,
					AutoJumpDuckAfter >= 0.0f ? TEXT(" (ducked in mid air)") : TEXT(""));
			}
		}
	}

	// speedlog.auto: the numbers surf and ladders are judged by.
	if (AutoSpeedLogSeconds > 0.0f)
	{
		AutoSpeedLogTimer -= DeltaSeconds;
		if (AutoSpeedLogTimer <= 0.0f)
		{
			AutoSpeedLogTimer = 0.25f;
			AutoSpeedLogSeconds -= 0.25f;
			const float Scale = ULambdaSourceSettings::Get().UnitScale;
			const UCharacterMovementComponent* Move = GetCharacterMovement();
			UE_LOG(LogLambda, Display, TEXT("speed: %.0f u/s (z %+.0f) feet %.0f u %s"),
				Move->Velocity.Size2D() / Scale, Move->Velocity.Z / Scale,
				(GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight()) / Scale,
				Move->MovementMode == MOVE_Custom ? TEXT("LADDER") : (Move->IsFalling() ? TEXT("air") : TEXT("ground")));
		}
	}

	// crouch.auto: hold duck, so the ducked hull and view can be tested.
	if (AutoCrouchSeconds > 0.0f)
	{
		if (AutoCrouchDelay > 0.0f)
		{
			AutoCrouchDelay -= DeltaSeconds;
		}
		else
		{
			if (!bIsCrouched)
			{
				// The resize happens in the movement update, not here, so there is nothing to report yet.
				Crouch();
				UE_LOG(LogLambda, Display, TEXT("crouch.auto: ducking"));
			}
			AutoCrouchSeconds -= DeltaSeconds;
			if (AutoCrouchSeconds <= 0.0f)
			{
				UnCrouch();
				UE_LOG(LogLambda, Display, TEXT("crouch.auto: standing up"));
			}
		}
	}

	// walk.auto: walk forward, so what the player bumps into can be tested.
	if (AutoWalkSeconds > 0.0f)
	{
		if (AutoWalkDelay > 0.0f)
		{
			AutoWalkDelay -= DeltaSeconds;
		}
		else
		{
			AutoWalkSeconds -= DeltaSeconds;
			AddMovementInput(FRotator(0.0f, GetControlRotation().Yaw, 0.0f).Vector(), 1.0f);
		}
	}

	// propcarry.auto: grab the prop ahead, hold it, then throw it.
	if (AutoCarryGrabTimer > 0.0f)
	{
		AutoCarryGrabTimer -= DeltaSeconds;
		if (AutoCarryGrabTimer <= 0.0f)
		{
			if (GEngine)
			{
				GEngine->Exec(GetWorld(), TEXT("prop_list"));
			}
			TogglePropCarry();
			// Look somewhere once it is in hand: level by default, or wherever the third argument asks for.
			if (CarriedProp.IsValid())
			{
				if (AController* PC = GetController())
				{
					FRotator View = PC->GetControlRotation();
					View.Pitch = AutoCarryLookPitch;
					PC->SetControlRotation(View);
				}
			}
			UE_LOG(LogLambda, Display, TEXT("propcarry: %s"), CarriedProp.IsValid() ? TEXT("carrying") : TEXT("nothing to carry"));
		}
	}
	else if (AutoCarryThrowTimer > 0.0f)
	{
		AutoCarryThrowTimer -= DeltaSeconds;
		if (AutoCarryThrowTimer <= 0.0f)
		{
			UE_LOG(LogLambda, Display, TEXT("propcarry: throwing (%s)"), ThrowCarriedProp() ? TEXT("ok") : TEXT("nothing held"));
		}
	}

	UpdatePropCarry(DeltaSeconds);

	// CalcPlayerView: the punch angle rides on top of the view angles and springs back (DecayPunchAngle).
	DecayPunchAngle(DeltaSeconds);
	if (FirstPersonCamera)
	{
		FirstPersonCamera->SetWorldRotation(GetControlRotation() + FRotator(PunchAngle.X, PunchAngle.Y, PunchAngle.Z));
	}

	UpdateMuzzleFlash();

	// Scripted-launch aids (npc_create.auto / decaltest.auto): -ExecCmds applies its cvars *after*
	// BeginPlay, so they are read here, once, two seconds into play.
	if (!bAutoCommandsRun)
	{
		AutoCommandTimer += DeltaSeconds;
		if (AutoCommandTimer >= 2.0f)
		{
			bAutoCommandsRun = true;
			if (!GDecalTestAuto.IsEmpty())
			{
				TArray<FString> Parts;
				GDecalTestAuto.ParseIntoArrayWS(Parts);
				RunDecalTest(Parts.Num() > 0 ? FCString::Atof(*Parts[0]) : 90.0f, Parts.Num() > 1 ? FCString::Atof(*Parts[1]) : 45.0f,
					3, Parts.Num() > 2 ? Parts[2] : FString());
			}
			if (!GSetPosAuto.IsEmpty())
			{
				TArray<FString> Parts;
				GSetPosAuto.ParseIntoArrayWS(Parts);
				if (Parts.Num() >= 3)
				{
					const FVector3f Source(FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]), FCString::Atof(*Parts[2]));
					const FVector Feet = FSourceCoords::ToUE(Source, ULambdaSourceSettings::Get().UnitScale);
					const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
					SetActorLocation(Feet + FVector(0, 0, HalfHeight + 2.0f), false, nullptr, ETeleportType::TeleportPhysics);
					// setang: the two extra arguments aim the view, Source-style (yaw first, then pitch). The
					// rotation is held for a moment - a single SetControlRotation can lose to the controller's
					// own first-frame update, and a spawn command reading the view that frame went the wrong way.
					if (Parts.Num() >= 4)
					{
						const FVector3f Angles(Parts.Num() >= 5 ? FCString::Atof(*Parts[4]) : 0.0f, FCString::Atof(*Parts[3]), 0.0f);
						AutoViewRotation = FSourceCoords::AnglesToUE(Angles);
						AutoViewHoldSeconds = 0.5f;
						if (AController* PC = GetController())
						{
							PC->SetControlRotation(AutoViewRotation);
						}
					}
					UE_LOG(LogLambda, Display, TEXT("setpos %s"), *GetActorLocation().ToString());
				}
			}
			if (!GPlayerModelAuto.IsEmpty())
			{
				SetPlayerModel(GPlayerModelAuto);
			}
			if (!GUseAuto.IsEmpty())
			{
				TArray<FString> Parts;
				GUseAuto.ParseIntoArrayWS(Parts);
				for (int32 i = 0; i < Parts.Num() && i < 2; ++i)
				{
					FTimerHandle Handle;
					GetWorldTimerManager().SetTimer(Handle,
						FTimerDelegate::CreateUObject(this, &ALambdaCharacter::Input_Use),
						FMath::Max(0.01f, FCString::Atof(*Parts[i])), false);
				}
			}
			if (!GShotAuto.IsEmpty())
			{
				TArray<FString> Parts;
				GShotAuto.ParseIntoArrayWS(Parts);
				const FString ShotName = Parts.Num() > 1 ? Parts[1] : TEXT("grab");
				FTimerHandle Handle;
				GetWorldTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([ShotName]()
				{
					// The automation path rather than the HighResShot console command, which writes nothing in
					// -game. The name is ours, so two runs can be told apart without guessing at a counter.
					FScreenshotRequest::RequestScreenshot(ShotName, false, false);
					UE_LOG(LogLambda, Display, TEXT("grab.auto: requested %s"), *ShotName);
				}), FMath::Max(0.01f, Parts.Num() > 0 ? FCString::Atof(*Parts[0]) : 5.0f), false);
			}
			if (!GEntFireAuto.IsEmpty())
			{
				// Several inputs at once, separated by '|', so one run can walk a whole sequence - turn a light
				// on, give it a pattern, fade it to another - instead of needing a launch apiece. Not ';':
				// the console splits its own command lines on that before a cvar ever sees it.
				TArray<FString> Commands;
				GEntFireAuto.ParseIntoArray(Commands, TEXT("|"), true);
				for (TActorIterator<ASourceBSPWorldActor> It(GetWorld()); It; ++It)
				{
					for (const FString& Command : Commands)
					{
						TArray<FString> Parts;
						Command.ParseIntoArrayWS(Parts);
						if (Parts.Num() < 2)
						{
							continue;
						}
						// A lone '-' stands in for "no parameter", so an input that takes none can still be
						// given a delay without an empty argument the command line cannot carry.
						FString Parameter = Parts.Num() > 2 ? Parts[2] : FString();
						if (Parameter == TEXT("-"))
						{
							Parameter.Reset();
						}
						const float Delay = Parts.Num() > 3 ? FCString::Atof(*Parts[3]) : 0.0f;
						It->QueueEntityEvent(Parts[0], Parts[1], Parameter, this, this, Delay);
						UE_LOG(LogLambda, Display, TEXT("entfire.auto %s %s '%s' in %gs"),
							*Parts[0], *Parts[1], *Parameter, Delay);
					}
					break;
				}
			}
			AutoSpawnDelay = 0.25f;
			PendingNPCCreate = GNPCCreateAuto;
			PendingPropCreate = GPropCreateAuto;
			if (!GPitchSweepAuto.IsEmpty())
			{
				TArray<FString> Parts;
				GPitchSweepAuto.ParseIntoArrayWS(Parts);
				if (Parts.Num() >= 3)
				{
					SweepFromPitch = -FCString::Atof(*Parts[0]);
					SweepToPitch = -FCString::Atof(*Parts[1]);
					SweepSeconds = FCString::Atof(*Parts[2]);
					SweepDelay = Parts.Num() > 3 ? FCString::Atof(*Parts[3]) : 8.0f;
					SweepElapsed = 0.0f;
				}
			}
			if (!GSlotAuto.IsEmpty())
			{
				TArray<FString> Parts;
				GSlotAuto.ParseIntoArrayWS(Parts);
				AutoSlotBucket = Parts.Num() > 0 ? FCString::Atoi(*Parts[0]) : 0;
				AutoSlotDelay = Parts.Num() > 1 ? FCString::Atof(*Parts[1]) : 1.0f;
			}
			if (!GThirdPersonAuto.IsEmpty())
			{
				TArray<FString> Parts;
				GThirdPersonAuto.ParseIntoArrayWS(Parts);
				AutoThirdPersonDelay = FMath::Max(0.01f, Parts.Num() > 0 ? FCString::Atof(*Parts[0]) : 4.0f);
				AutoFirstPersonDelay = Parts.Num() > 1 ? FCString::Atof(*Parts[1]) : 0.0f;
			}
			if (!GHurtAuto.IsEmpty())
			{
				TArray<FString> Parts;
				GHurtAuto.ParseIntoArrayWS(Parts);
				AutoHurtAmount = Parts.Num() > 0 ? FCString::Atof(*Parts[0]) : 10.0f;
				AutoHurtType = Parts.Num() > 1 ? Parts[1] : TEXT("generic");
				AutoHurtDelay = Parts.Num() > 2 ? FCString::Atof(*Parts[2]) : 2.0f;
			}
			if (!GWalkAuto.IsEmpty())
			{
				TArray<FString> Parts;
				GWalkAuto.ParseIntoArrayWS(Parts);
				AutoWalkSeconds = Parts.Num() > 0 ? FCString::Atof(*Parts[0]) : 2.0f;
				AutoWalkDelay = Parts.Num() > 1 ? FCString::Atof(*Parts[1]) : 2.0f;
			}
			if (!GJumpAuto.IsEmpty())
			{
				TArray<FString> Parts;
				GJumpAuto.ParseIntoArrayWS(Parts);
				AutoJumpDelay = Parts.Num() > 0 ? FCString::Atof(*Parts[0]) : 1.0f;
				AutoJumpDuckAfter = Parts.Num() > 1 ? FCString::Atof(*Parts[1]) : -1.0f;
				bAutoJumpArmed = true;
			}
			if (!GSpeedLogAuto.IsEmpty())
			{
				AutoSpeedLogSeconds = FCString::Atof(*GSpeedLogAuto);
			}
			if (!GCrouchAuto.IsEmpty())
			{
				TArray<FString> Parts;
				GCrouchAuto.ParseIntoArrayWS(Parts);
				AutoCrouchSeconds = Parts.Num() > 0 ? FCString::Atof(*Parts[0]) : 2.0f;
				AutoCrouchDelay = Parts.Num() > 1 ? FCString::Atof(*Parts[1]) : 2.0f;
			}
			if (!GPropCarryAuto.IsEmpty())
			{
				TArray<FString> Parts;
				GPropCarryAuto.ParseIntoArrayWS(Parts);
				AutoCarryGrabTimer = Parts.Num() > 0 ? FCString::Atof(*Parts[0]) : 2.0f;
				AutoCarryThrowTimer = Parts.Num() > 1 ? FCString::Atof(*Parts[1]) : -1.0f;
				// Source pitch: positive looks down, so the view can be aimed under the horizon for a test.
				AutoCarryLookPitch = Parts.Num() > 2 ? -FCString::Atof(*Parts[2]) : 0.0f;
			}
			if (!GFireHoldAuto.IsEmpty())
			{
				TArray<FString> Parts;
				GFireHoldAuto.ParseIntoArrayWS(Parts);
				AutoFireHoldLeft = Parts.Num() > 0 ? FCString::Atof(*Parts[0]) : 1.0f;
				AutoFireHoldDelay = Parts.Num() > 1 ? FCString::Atof(*Parts[1]) : 2.0f;
				bAutoFireHoldAlt = Parts.Num() > 2 && Parts[2].Equals(TEXT("alt"), ESearchCase::IgnoreCase);
			}
			if (!GFireAuto.IsEmpty())
			{
				TArray<FString> Parts;
				GFireAuto.ParseIntoArrayWS(Parts);
				AutoFireShotsLeft = Parts.Num() > 0 ? FCString::Atoi(*Parts[0]) : 1;
				AutoFireInterval = Parts.Num() > 1 ? FCString::Atof(*Parts[1]) : 0.6f;
				AutoFireTimer = -(Parts.Num() > 2 ? FCString::Atof(*Parts[2]) : 2.0f);
				bAutoFireAimHead = Parts.Num() > 3 && Parts[3].Equals(TEXT("head"), ESearchCase::IgnoreCase);
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

	// firehold.auto: the trigger simply stays down, so an automatic weapon runs as it would in the hand.
	if (AutoFireHoldLeft > 0.0f)
	{
		if (AutoFireHoldDelay > 0.0f)
		{
			AutoFireHoldDelay -= DeltaSeconds;
		}
		else if (ActiveWeapon)
		{
			if (!bAutoFireHolding)
			{
				bAutoFireHolding = true;
				(bAutoFireHoldAlt ? ActiveWeapon->bAttack2PressedThisFrame : ActiveWeapon->bAttackPressedThisFrame) = true;
				UE_LOG(LogLambda, Display, TEXT("firehold.auto: %s trigger down for %.2fs"),
					bAutoFireHoldAlt ? TEXT("second") : TEXT("first"), AutoFireHoldLeft);
			}
			(bAutoFireHoldAlt ? ActiveWeapon->bAttack2Held : ActiveWeapon->bAttackHeld) = true;
			AutoFireHoldLeft -= DeltaSeconds;
			if (AutoFireHoldLeft <= 0.0f)
			{
				ActiveWeapon->bAttackHeld = false;
				ActiveWeapon->bAttack2Held = false;
				bAutoFireHolding = false;
				UE_LOG(LogLambda, Display, TEXT("firehold.auto: trigger released, clip %d"), ActiveWeapon->GetClip1());
			}
		}
	}

	// fire.auto: a one-frame trigger pull per shot (the pistol is semi-automatic), screenshot shortly after.
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
			// aim at whatever npc_create.auto spawned, so the scripted shots land on it - or on its corpse
			if (AutoFireTarget.IsValid() && Controller)
			{
				FVector Eye;
				FRotator EyeRot;
				GetActorEyesViewPoint(Eye, EyeRot);
				FVector Aim = AutoFireTarget->GetActorLocation();
				if (const ASourceNPCBase* NPC = Cast<ASourceNPCBase>(AutoFireTarget.Get()))
				{
					if (bAutoFireAimHead)
					{
						Aim = NPC->EyePosition();
					}
					if (const ASourceRagdoll* Ragdoll = NPC->GetRagdoll())
					{
						Aim = Ragdoll->GetAimPoint();
					}
				}
				Controller->SetControlRotation((Aim - Eye).Rotation());
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

ALambdaWeapon* ALambdaCharacter::FindWeapon(const FString& WeaponClassName) const
{
	for (const TObjectPtr<ALambdaWeapon>& Weapon : Weapons)
	{
		if (Weapon && Weapon->GetWeaponClassName().Equals(WeaponClassName, ESearchCase::IgnoreCase))
		{
			return Weapon.Get();
		}
	}
	return nullptr;
}

void ALambdaCharacter::SwitchToWeapon(ALambdaWeapon* Weapon)
{
	if (!Weapon || Weapon == ActiveWeapon)
	{
		return;
	}
	if (ActiveWeapon)
	{
		ActiveWeapon->Holster();
		LastWeapon = ActiveWeapon;
	}
	ActiveWeapon = Weapon;
	SetViewModel(Weapon->GetWeaponInfo().ViewModel);
	Weapon->Deploy();
}

void ALambdaCharacter::Input_LastInv()
{
	// "lastinv" swaps straight back, no menu.
	bSelectionActive = false;
	if (LastWeapon && Weapons.Contains(LastWeapon))
	{
		SwitchToWeapon(LastWeapon);
	}
}

void ALambdaCharacter::SelectSlot(int32 Bucket)
{
	// CHudWeaponSelection::SelectWeaponSlot: open the menu on that bucket, or cycle within it if already there.
	int32 First = INDEX_NONE;
	for (int32 i = 0; i < Weapons.Num(); ++i)
	{
		if (Weapons[i] && Weapons[i]->GetWeaponInfo().Bucket == Bucket)
		{
			First = i;
			break;
		}
	}
	if (First == INDEX_NONE)
	{
		// An empty slot is refused out loud, the way Source refuses it.
		PlayUISound(TEXT("Player.DenyWeaponSelection"));
		return;
	}
	const int32 Was = bSelectionActive ? SelectionIndex : INDEX_NONE;
	if (bSelectionActive && Weapons.IsValidIndex(SelectionIndex) && Weapons[SelectionIndex]->GetWeaponInfo().Bucket == Bucket)
	{
		// Cycle within the bucket, wrapping back to its first weapon.
		int32 Next = SelectionIndex + 1;
		if (!Weapons.IsValidIndex(Next) || Weapons[Next]->GetWeaponInfo().Bucket != Bucket)
		{
			Next = First;
		}
		SelectionIndex = Next;
	}
	else
	{
		SelectionIndex = First;
	}
	bSelectionActive = true;
	if (SelectionIndex != Was)
	{
		PlayUISound(TEXT("Player.WeaponSelectionMoveSlot"));
	}
}

void ALambdaCharacter::ConfirmWeaponSelection()
{
	bSelectionActive = false;
	PlayUISound(TEXT("Player.WeaponSelected"));
	SwitchToWeapon(GetSelectedWeapon());
}

void ALambdaCharacter::CycleSelection(int32 Step)
{
	// invnext/invprev walk the whole arsenal in bucket order, opening the menu from the weapon in hand.
	if (Weapons.Num() == 0)
	{
		return;
	}
	if (!bSelectionActive)
	{
		SelectionIndex = Weapons.IndexOfByKey(ActiveWeapon);
		bSelectionActive = true;
	}
	const int32 Was = SelectionIndex;
	SelectionIndex = (SelectionIndex + Step + Weapons.Num()) % Weapons.Num();
	if (SelectionIndex != Was)
	{
		// CHudWeaponSelection::CycleToNextWeapon - every notch of the wheel is a click.
		PlayUISound(TEXT("Player.WeaponSelectionMoveSlot"));
	}
}

ALambdaWeapon* ALambdaCharacter::GiveWeapon(const FString& WeaponClassName)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	// CBasePlayer::GiveNamedItem: "If I already own this type don't create one." BumpWeapon checks before it
	// gets here, but the give console command does not, and a second crowbar shows up as a second slot in the
	// weapon selection for the rest of the game.
	if (ALambdaWeapon* Existing = FindWeapon(WeaponClassName))
	{
		return Existing;
	}

	// Pick the class that implements this weapon; anything without a dedicated port still gets the shared behaviour.
	TSubclassOf<ALambdaWeapon> WeaponClass = ALambdaWeapon::StaticClass();
	if (WeaponClassName.Equals(TEXT("weapon_pistol"), ESearchCase::IgnoreCase))
	{
		WeaponClass = ALambdaWeaponPistol::StaticClass();
	}
	else if (WeaponClassName.Equals(TEXT("weapon_crowbar"), ESearchCase::IgnoreCase))
	{
		WeaponClass = ALambdaWeaponCrowbar::StaticClass();
	}
	else if (WeaponClassName.Equals(TEXT("weapon_smg1"), ESearchCase::IgnoreCase))
	{
		WeaponClass = ALambdaWeaponSMG1::StaticClass();
	}
	else if (WeaponClassName.Equals(TEXT("weapon_shotgun"), ESearchCase::IgnoreCase))
	{
		WeaponClass = ALambdaWeaponShotgun::StaticClass();
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

	// The arsenal, kept sorted by bucket then position - the order the selection HUD draws it in.
	Weapons.Add(Weapon);
	Weapons.Sort([](const ALambdaWeapon& A, const ALambdaWeapon& B)
	{
		const FSourceWeaponInfo& IA = A.GetWeaponInfo();
		const FSourceWeaponInfo& IB = B.GetWeaponInfo();
		return IA.Bucket != IB.Bucket ? IA.Bucket < IB.Bucket : IA.BucketPosition < IB.BucketPosition;
	});

	// CBasePlayer::Weapon_Equip switches to a picked-up weapon.
	SwitchToWeapon(Weapon);
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
	// Applied here rather than in the constructor so it follows the model: the primitive type is part of how
	// this component is drawn, and it has to be re-stated after the mesh changes.
	ViewModelMesh->SetFirstPersonPrimitiveType(
		GLambdaViewModelFirstPerson ? EFirstPersonPrimitiveType::FirstPerson : EFirstPersonPrimitiveType::None);

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

bool ALambdaCharacter::BumpWeapon(const FString& WeaponClassName)
{
	// CBasePlayer::BumpWeapon: a weapon that is already carried hands over its ammo and stays on the floor for
	// nobody; a new one is taken and becomes the active weapon (Weapon_Switch), carrying the clip a map-placed
	// weapon comes with.
	const FSourceWeaponInfo* Info = FSourceWeaponScripts::Get().Find(WeaponClassName);
	if (!Info)
	{
		return false;
	}

	// A weapon already carried hands over its ammo instead.
	if (FindWeapon(WeaponClassName))
	{
		return GiveAmmo(Info->PrimaryAmmo, FMath::Max(Info->ClipSize, 1)) > 0;
	}

	if (GiveWeapon(WeaponClassName))
	{
		GiveAmmo(Info->PrimaryAmmo, FMath::Max(Info->ClipSize, 1));
		ALambdaCharacterAddPickupHistory(PickupHistory, GetWorld(), WeaponClassName.ToUpper());
		return true;
	}
	return false;
}

void ALambdaCharacterAddPickupHistory(TArray<ALambdaCharacter::FPickupEvent>& History, UWorld* World, const FString& Text)
{
	ALambdaCharacter::FPickupEvent& Event = History.AddDefaulted_GetRef();
	Event.Text = Text;
	Event.Time = World ? World->GetTimeSeconds() : 0.0f;
	while (History.Num() > 4)
	{
		History.RemoveAt(0);
	}
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
	const int32 Added = Current - Before;
	if (Added > 0)
	{
		// CHudHistoryResource: the pickup shows on the right of the screen for a moment.
		ALambdaCharacterAddPickupHistory(PickupHistory, GetWorld(),
			FString::Printf(TEXT("+%d %s"), Added, *AmmoType.ToUpper()));
	}
	return Added;
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

void ALambdaCharacter::Input_Attack2Start()
{
	if (ActiveWeapon)
	{
		ActiveWeapon->bAttack2Held = true;
		ActiveWeapon->bAttack2PressedThisFrame = true;
	}
}

void ALambdaCharacter::Input_Attack2Stop()
{
	if (ActiveWeapon)
	{
		ActiveWeapon->bAttack2Held = false;
	}
}

void ALambdaCharacter::Input_AttackStart()
{
	// With the weapon menu open, the attack is the confirmation, not a shot (CBaseHudWeaponSelection).
	if (bSelectionActive)
	{
		ConfirmWeaponSelection();
		return;
	}
	// CPlayerPickupController::Use: firing while carrying something throws it instead (the weapon is holstered
	// while the player has his hands full).
	if (ThrowCarriedProp())
	{
		return;
	}
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
	TEXT("muzzleflash.holdtime"),
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

	if (bThirdPerson)
	{
		return;	// the flash belongs to the view model, and in third person there is no view model to flash
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

void ALambdaCharacter::ViewPunch(const FRotator& AngleOffset)
{
	// CBasePlayer::ViewPunch: m_Local.m_vecPunchAngleVel += angleOffset * 20
	PunchAngleVel += FVector(AngleOffset.Pitch, AngleOffset.Yaw, AngleOffset.Roll) * 20.0f;
}

void ALambdaCharacter::VelocityPunch(const FVector& VelocityImpulse)
{
	// CBaseCombatCharacter::VelocityPunch: SetGroundEntity(NULL) + ApplyAbsVelocityImpulse
	LaunchCharacter(VelocityImpulse, false, false);
}

void ALambdaCharacter::DecayPunchAngle(float DeltaSeconds)
{
	// CBasePlayer::DecayPunchAngle, PUNCH_DAMPING 9 and PUNCH_SPRING_CONSTANT 65
	if (PunchAngle.SizeSquared() > 0.001f || PunchAngleVel.SizeSquared() > 0.001f)
	{
		PunchAngle += PunchAngleVel * DeltaSeconds;
		float Damping = 1.0f - (9.0f * DeltaSeconds);
		if (Damping < 0.0f) { Damping = 0.0f; }
		PunchAngleVel *= Damping;
		// torsional spring
		float SpringForceMagnitude = 65.0f * DeltaSeconds;
		SpringForceMagnitude = FMath::Clamp(SpringForceMagnitude, 0.0f, 2.0f);
		PunchAngleVel -= PunchAngle * SpringForceMagnitude;
		// don't wrap around
		PunchAngle.X = FMath::Clamp(PunchAngle.X, -89.0f, 89.0f);
		PunchAngle.Y = FMath::Clamp(PunchAngle.Y, -179.0f, 179.0f);
		PunchAngle.Z = FMath::Clamp(PunchAngle.Z, -89.0f, 89.0f);
	}
	else
	{
		PunchAngle = FVector::ZeroVector;
		PunchAngleVel = FVector::ZeroVector;
	}
}

void ALambdaCharacter::EquipSuit(bool bEquip)
{
	if (bSuitEquipped == bEquip)
	{
		return;
	}
	bSuitEquipped = bEquip;
	if (bEquip)
	{
		// The logon is the one line that jumps the queue - it is what the suit says as it comes up.
		SuitVoice.SetSuitUpdate(this, TEXT("HEV_LOGON"), FLambdaSuitVoice::RepeatOK);
	}
	else
	{
		SuitVoice.Reset();
	}
}

void ALambdaCharacter::GiveArmor(float Amount)
{
	// IncrementArmorValue( nCount, MAX_NORMAL_BATTERY ).
	Armor = FMath::Clamp(Armor + Amount, 0.0f, 100.0f);
}

float ALambdaCharacter::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	if (DamageAmount <= 0.0f || !IsAlive())
	{
		return 0.0f;
	}
	// CHL1_Player::OnTakeDamage. The damage type is what tells the suit which injury this is; damage from
	// something that never filled it in is generic, and the suit stays quiet about it.
	const int32 DamageType = DamageEvent.IsOfType(FSourceDamageEvent::ClassID)
		? static_cast<const FSourceDamageEvent&>(DamageEvent).DamageType
		: SourceDamageType::DMG_GENERIC;
	const float HealthPrev = Health;

	// ---- armour ----
	//
	// ARMOR_RATIO 0.2 / ARMOR_BONUS 0.5: health takes a fifth of the blow and the suit soaks up the rest, at
	// two points of damage stopped for every point of armour spent. It does not help against a fall, drowning
	// or poison - those do not arrive at the surface of the suit.
	float Damage = DamageAmount;
	using namespace SourceDamageType;
	if (Armor > 0.0f && !(DamageType & (DMG_FALL | DMG_DROWN | DMG_POISON)))
	{
		constexpr float ArmorRatio = 0.2f;
		constexpr float ArmorBonus = 0.5f;
		float ToHealth = Damage * ArmorRatio;
		float ToArmor = (Damage - ToHealth) * ArmorBonus;
		if (ToArmor > Armor)
		{
			// The armour runs out part way through and the remainder lands on health.
			ToArmor = Armor * (1.0f / ArmorBonus);
			ToHealth = Damage - ToArmor;
			Armor = 0.0f;
		}
		else
		{
			Armor -= ToArmor;
		}
		Damage = ToHealth;
	}

	Health = FMath::Max(0.0f, Health - Damage);
	UE_LOG(LogLambda, Log, TEXT("player took %.0f damage (%.0f after armour) from %s, health %.0f armour %.0f"),
		DamageAmount, Damage, *GetNameSafe(DamageCauser), Health, Armor);

	// m_bitsDamageType: what is hurting, for the HUD's icons.
	DamageBits |= DamageType;
	DamageBitsTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;

	SuitDamageReaction(DamageType, Damage, HealthPrev);

	// The HUD's damage indicator wants to know which way the blow came from, relative to the view.
	LastDamageTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	LastDamageAmount = DamageAmount;
	if (DamageCauser)
	{
		const FVector ToCauser = (DamageCauser->GetActorLocation() - GetActorLocation()).GetSafeNormal2D();
		LastDamageYaw = FMath::FindDeltaAngleDegrees(GetControlRotation().Yaw, ToCauser.Rotation().Yaw);
	}
	else
	{
		LastDamageYaw = 0.0f;
	}

	if (!IsAlive())
	{
		Killed(DamageCauser);
	}
	return DamageAmount;
}

void ALambdaCharacter::SuitDamageReaction(int32 DamageType, float Damage, float HealthPrev)
{
	// CHL1_Player::OnTakeDamage's suit half, verbatim in structure: the suit names the injury, then - if this
	// was a real blow rather than a scratch - comments on how much of the player is left.
	//
	// Source loops here clearing one damage bit at a time because a single blow can carry several. The loop is
	// kept: a shotgun to the face underwater really does have three things to say about it.
	if (!bSuitEquipped)
	{
		return;
	}
	using namespace SourceDamageType;

	const bool bTrivial = (Health > 75.0f || Damage < 5.0f);
	const bool bMajor = (Damage > 25.0f);
	const bool bCritical = (Health < 30.0f);
	const bool bTimeBased = (DamageType & DMG_TIMEBASED) != 0;

	int32 Bits = DamageType;
	while ((!bTrivial || bTimeBased) && Bits != 0)
	{
		const int32 Before = Bits;

		if (Bits & DMG_CLUB)
		{
			if (bMajor) { SuitVoice.SetSuitUpdate(this, TEXT("HEV_DMG4"), FLambdaSuitVoice::NextIn30Sec); }
			Bits &= ~DMG_CLUB;
		}
		if (Bits & (DMG_FALL | DMG_CRUSH))
		{
			SuitVoice.SetSuitUpdate(this, bMajor ? TEXT("HEV_DMG5") : TEXT("HEV_DMG4"), FLambdaSuitVoice::NextIn30Sec);
			Bits &= ~(DMG_FALL | DMG_CRUSH);
		}
		if (Bits & (DMG_BULLET | DMG_BUCKSHOT))
		{
			if (Damage > 5.0f) { SuitVoice.SetSuitUpdate(this, TEXT("HEV_DMG6"), FLambdaSuitVoice::NextIn30Sec); }
			Bits &= ~(DMG_BULLET | DMG_BUCKSHOT);
		}
		if (Bits & DMG_SLASH)
		{
			SuitVoice.SetSuitUpdate(this, bMajor ? TEXT("HEV_DMG1") : TEXT("HEV_DMG0"), FLambdaSuitVoice::NextIn30Sec);
			Bits &= ~DMG_SLASH;
		}
		if (Bits & DMG_SONIC)
		{
			if (bMajor) { SuitVoice.SetSuitUpdate(this, TEXT("HEV_DMG2"), FLambdaSuitVoice::NextIn1Min); }
			Bits &= ~DMG_SONIC;
		}
		if (Bits & (DMG_POISON | DMG_PARALYZE))
		{
			SuitVoice.SetSuitUpdate(this, TEXT("HEV_DMG3"), FLambdaSuitVoice::NextIn1Min);
			Bits &= ~(DMG_POISON | DMG_PARALYZE);
		}
		if (Bits & DMG_ACID)
		{
			SuitVoice.SetSuitUpdate(this, TEXT("HEV_DET1"), FLambdaSuitVoice::NextIn1Min);
			Bits &= ~DMG_ACID;
		}
		if (Bits & DMG_NERVEGAS)
		{
			SuitVoice.SetSuitUpdate(this, TEXT("HEV_DET0"), FLambdaSuitVoice::NextIn1Min);
			Bits &= ~DMG_NERVEGAS;
		}
		if (Bits & DMG_RADIATION)
		{
			SuitVoice.SetSuitUpdate(this, TEXT("HEV_DET2"), FLambdaSuitVoice::NextIn1Min);
			Bits &= ~DMG_RADIATION;
		}
		Bits &= ~DMG_SHOCK;

		if (Bits == Before)
		{
			break;	// nothing here the suit knows a word for
		}
	}

	// A first real wound, while still healthy: the automedic.
	if (!bTrivial && bMajor && HealthPrev >= 75.0f)
	{
		SuitVoice.SetSuitUpdate(this, TEXT("HEV_MED1"), FLambdaSuitVoice::NextIn30Min);
		SuitVoice.SetSuitUpdate(this, TEXT("HEV_HEAL7"), FLambdaSuitVoice::NextIn30Min);
	}
	// Getting dangerous.
	if (!bTrivial && bCritical && HealthPrev < 75.0f)
	{
		if (Health < 6.0f)
		{
			SuitVoice.SetSuitUpdate(this, TEXT("HEV_HLTH3"), FLambdaSuitVoice::NextIn10Min);
		}
		else if (Health < 20.0f)
		{
			SuitVoice.SetSuitUpdate(this, TEXT("HEV_HLTH2"), FLambdaSuitVoice::NextIn10Min);
		}
		if (FMath::RandRange(0, 3) == 0 && HealthPrev < 50.0f)
		{
			SuitVoice.SetSuitUpdate(this, TEXT("HEV_DMG7"), FLambdaSuitVoice::NextIn5Min);
		}
	}
	if (bTimeBased && HealthPrev < 75.0f)
	{
		if (HealthPrev < 50.0f)
		{
			if (FMath::RandRange(0, 3) == 0)
			{
				SuitVoice.SetSuitUpdate(this, TEXT("HEV_DMG7"), FLambdaSuitVoice::NextIn5Min);
			}
		}
		else
		{
			SuitVoice.SetSuitUpdate(this, TEXT("HEV_HLTH1"), FLambdaSuitVoice::NextIn10Min);
		}
	}
}

void ALambdaCharacter::Killed(AActor* Attacker)
{
	UE_LOG(LogLambda, Log, TEXT("player killed by %s"), *GetNameSafe(Attacker));

	// The flatline is the last thing the suit says, and it says it over everything else that was queued.
	SuitVoice.Reset();
	if (bSuitEquipped)
	{
		SuitVoice.SetSuitUpdate(this, FMath::RandBool() ? TEXT("HEV_DEAD0") : TEXT("HEV_DEAD1"), FLambdaSuitVoice::RepeatOK);
	}

	// CBasePlayer::Event_Killed drops the weapon and hands control to the death camera. Neither exists yet, so
	// the player is simply frozen where they fell - enough that death reads as death rather than as nothing.
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		DisableInput(PC);
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

AActor* ALambdaCharacter::PropCreate(const FString& ModelPath, float MaxDistanceCm)
{
	UWorld* World = GetWorld();
	ASourceBSPWorldActor* BSPWorld = World ? Cast<ASourceBSPWorldActor>(UGameplayStatics::GetActorOfClass(World, ASourceBSPWorldActor::StaticClass())) : nullptr;
	if (!BSPWorld)
	{
		UE_LOG(LogLambda, Warning, TEXT("prop_create: no BSP world actor"));
		return nullptr;
	}
	// prop_physics_create drops the prop where the player is looking, a little off the surface so it falls in.
	FVector Eye;
	FRotator EyeRot;
	GetActorEyesViewPoint(Eye, EyeRot);
	const FVector Forward = EyeRot.Vector();
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(LambdaPropCreate), true, this);
	// CreatePhysicsProp sweeps the model's own hull and sets the prop a unit off whatever it hit. The hull is not
	// known before the model is loaded, so the prop is dropped a short way clear of the surface and left to settle.
	FVector Spot = Eye + Forward * MaxDistanceCm;
	if (World->LineTraceSingleByChannel(Hit, Eye, Spot, ECC_Visibility, Params))
	{
		Spot = Hit.ImpactPoint + Hit.ImpactNormal * 32.0f;
	}
	AActor* Prop = BSPWorld->CreateProp(ModelPath, Spot, 0.0f);
	UE_LOG(LogLambda, Display, TEXT("prop_create %s: %s"), *ModelPath, Prop ? *Prop->GetActorLocation().ToString() : TEXT("failed"));
	return Prop;
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
	// ...then UTIL_DropToFloor: the NPC stands on whatever is below that point. Not the barnacle - a ceiling
	// feeder hangs where the trace hit, so aiming at the ceiling puts one there.
	if (!ClassName.Equals(TEXT("npc_barnacle"), ESearchCase::IgnoreCase))
	{
		FHitResult Floor;
		if (World->LineTraceSingleByChannel(Floor, Spot + FVector(0, 0, 20.0f), Spot - FVector(0, 0, 5000.0f), ECC_Visibility, Params))
		{
			Spot = Floor.ImpactPoint + FVector(0, 0, 1.0f);
		}
	}
	else if (Hit.bBlockingHit)
	{
		// The barnacle's origin is its mount point: put it right at the ceiling, not a hull's width below it.
		Spot = Hit.ImpactPoint;
	}
	const float Yaw = (GetActorLocation() - Spot).Rotation().Yaw;
	AActor* NPC = BSPWorld->CreateNPC(ClassName, Spot, Yaw);
	UE_LOG(LogLambda, Display, TEXT("npc_create %s: %s"), *ClassName, NPC ? *NPC->GetActorLocation().ToString() : TEXT("failed"));
	return NPC;
}

void ALambdaCharacter::UpdateStepSound(float DeltaSeconds)
{
	// CBasePlayer::UpdateStepSound. The clock is in milliseconds because Source's is.
	if (StepSoundTime > 0.0f)
	{
		StepSoundTime = FMath::Max(0.0f, StepSoundTime - 1000.0f * DeltaSeconds);
	}
	if (StepSoundTime > 0.0f)
	{
		return;
	}

	const UCharacterMovementComponent* Move = GetCharacterMovement();
	if (!Move)
	{
		return;
	}

	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const float Speed = Move->Velocity.Size();
	const float GroundSpeed = Move->Velocity.Size2D();
	const bool bDucking = bIsCrouched;

	// GetStepSoundVelocities: how fast counts as walking, and how fast counts as running.
	const float VelWalk = (bDucking ? 60.0f : 90.0f) * Scale;
	const float VelRun = (bDucking ? 80.0f : 220.0f) * Scale;

	// You must be on the ground, moving along it, and moving fast enough to be worth a sound.
	if (Speed < VelWalk || GroundSpeed <= 0.0001f || !Move->IsMovingOnGround())
	{
		return;
	}

	const bool bWalking = Speed < VelRun;
	StepSoundTime = bWalking ? 400.0f : 300.0f;
	if (bDucking)
	{
		StepSoundTime += 100.0f;
	}

	// What we are standing on. Unreal's own floor result cannot answer this: it is a sweep, and a sweep does not
	// come back with the face it touched unless it was asked to, so its FaceIndex is always none and every step
	// would be the default surface. Source works the surface out with its own trace in CategorizePosition; this
	// is that trace.
	FString SurfaceProp;
	if (UWorld* World = GetWorld())
	{
		const FVector Feet = GetActorLocation() - FVector(0.0f, 0.0f, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
		FCollisionQueryParams Params(SCENE_QUERY_STAT(FootstepSurface), /*bTraceComplex=*/ true, this);
		Params.bReturnFaceIndex = true;

		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, Feet + FVector(0.0f, 0.0f, 8.0f * Scale),
			Feet - FVector(0.0f, 0.0f, 16.0f * Scale), ECC_Visibility, Params))
		{
			if (const USourceBrushMeshComponent* FloorMesh = Cast<USourceBrushMeshComponent>(Hit.GetComponent()))
			{
				if (Hit.FaceIndex != INDEX_NONE)
				{
					if (ULambdaMaterialLibrary* Materials = GetWorldMaterialLibrary())
					{
						SurfaceProp = Materials->GetSurfaceProp(FloorMesh->GetMaterialNameForFaceIndex(Hit.FaceIndex));
					}
				}
			}
		}
	}
	if (SurfaceProp.IsEmpty())
	{
		SurfaceProp = TEXT("default");
	}

	// The volumes Source picks per game material: most surfaces are quiet, dirt a little louder, a vent louder
	// still because it booms.
	float Volume = bWalking ? 0.2f : 0.5f;
	switch (FSourceSurfaceProps::Get().GetGameMaterial(SurfaceProp))
	{
	case 'D':	// CHAR_TEX_DIRT
		Volume = bWalking ? 0.25f : 0.55f;
		break;
	case 'V':	// CHAR_TEX_VENT
		Volume = bWalking ? 0.4f : 0.7f;
		break;
	default:
		break;
	}
	if (bDucking)
	{
		Volume *= 0.65f;
	}

	UE_LOG(LogLambda, Verbose, TEXT("footstep: %s at %.0f u/s (%s)"), *SurfaceProp, Speed / Scale,
		bWalking ? TEXT("walking") : TEXT("running"));
	PlayStepSound(SurfaceProp, Volume);
}

void ALambdaCharacter::PlayStepSound(const FString& SurfaceProp, float Volume)
{
	// CBasePlayer::PlayStepSound: the surface says what each foot sounds like, and the feet alternate.
	const FSourceSurfaceProp* Prop = FSourceSurfaceProps::Get().Find(SurfaceProp);
	if (!Prop)
	{
		return;
	}
	const FString& SoundName = bStepSide ? Prop->StepLeftSound : Prop->StepRightSound;
	bStepSide = !bStepSide;
	if (SoundName.IsEmpty())
	{
		return;
	}

	float ScriptVolume = 1.0f, Pitch = 1.0f;
	if (ULambdaSoundWave* Wave = FLambdaSoundCache::Get().CreateWaveResolved(this, SoundName, false, ScriptVolume, Pitch))
	{
		// Source passes its own volume rather than the script's here, and the script's soundlevel decides how
		// far it carries.
		const FSourceSoundScriptEntry* Entry = FSourceSoundScripts::Get().Find(SoundName);
		USoundAttenuation* Attenuation = FLambdaSoundCache::Get().GetAttenuationForSoundLevel(Entry ? Entry->SoundLevel : 75.0f);
		UGameplayStatics::SpawnSoundAtLocation(this, Wave, GetActorLocation(), FRotator::ZeroRotator,
			Volume, Pitch, 0.0f, Attenuation);
	}
}

void ALambdaCharacter::PlayUISound(const FString& ScriptName)
{
	float Volume = 1.0f, Pitch = 1.0f;
	ULambdaSoundWave* Wave = FLambdaSoundCache::Get().CreateWaveResolved(this, ScriptName, false, Volume, Pitch);
	UE_LOG(LogLambda, Verbose, TEXT("ui sound: %s%s"), *ScriptName, Wave ? TEXT("") : TEXT(" MISSING"));
	if (Wave)
	{
		UGameplayStatics::PlaySound2D(this, Wave, Volume, Pitch);
	}
}

void ALambdaCharacter::Input_CrouchStart()
{
	Crouch();
}

void ALambdaCharacter::Input_CrouchEnd()
{
	UnCrouch();
}

void ALambdaCharacter::OnStartCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
	Super::OnStartCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);

	// On the ground Unreal does what Source does: shrink the capsule and drop its centre so the feet stay put.
	// In the air it keeps the centre instead (bCrouchMaintainsBaseLocation is set false for every movement mode
	// but walking), so the feet already come up by half the hull difference on their own. Source lifts them by
	// the whole of it, so what is owed is the other half - one adjustment, not two.
	if (GetCharacterMovement() && GetCharacterMovement()->IsFalling())
	{
		AddActorWorldOffset(FVector(0.0f, 0.0f, ScaledHalfHeightAdjust), /*bSweep=*/ false);
		// The feet just moved the whole hull difference in one frame, so the view has to as well or the head
		// lurches. Only here, though - a duck begun on the ground eases even if a jump interrupts it, because
		// there the feet never moved.
		EyeAboveFeetCm = 28.0f * ULambdaSourceSettings::Get().UnitScale;	// VEC_DUCK_VIEW
	}
}

void ALambdaCharacter::OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
	Super::OnEndCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);

	// FinishUnDuck: standing up in mid air puts the feet back where they would have been, and again Unreal has
	// already done half of it by growing the capsule around a fixed centre.
	if (GetCharacterMovement() && GetCharacterMovement()->IsFalling())
	{
		AddActorWorldOffset(FVector(0.0f, 0.0f, -ScaledHalfHeightAdjust), /*bSweep=*/ false);
		EyeAboveFeetCm = 64.0f * ULambdaSourceSettings::Get().UnitScale;	// VEC_VIEW
	}
}

void ALambdaCharacter::UpdateEyeHeight(float DeltaSeconds)
{
	// VEC_VIEW / VEC_DUCK_VIEW: the eye is 64 units above the feet standing and 28 ducked, and the movement
	// component says how far through the duck transition the view is. Reading that rather than "am I crouched
	// yet" is what makes the crouch answer the key at once, and what keeps the head still when a duck finishes
	// in mid air - the fraction jumps to ducked in the same frame the feet come up.
	if (!FirstPersonCamera)
	{
		return;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const ULambdaCharacterMovement* Move = Cast<ULambdaCharacterMovement>(GetCharacterMovement());
	const float Fraction = Move ? Move->GetDuckViewFraction() : (bIsCrouched ? 1.0f : 0.0f);

	EyeAboveFeetCm = FMath::Lerp(64.0f * Scale, 28.0f * Scale, Fraction);

	// Stated from the feet, so it loses the half height to become a position relative to the middle of the
	// capsule - which is also what absorbs the capsule resizing under it, so nothing jumps.
	const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	FirstPersonCamera->SetRelativeLocation(FVector(0.0f, 0.0f, EyeAboveFeetCm - HalfHeight));
}

bool ALambdaCharacter::CanJumpInternal_Implementation() const
{
	// Everything Unreal asks except being crouched, which Source allows.
	return JumpIsAllowedInternal();
}
