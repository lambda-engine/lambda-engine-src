// The player's own body, seen and unseen.
//
// Half-Life 2 never draws the player. In first person you are a camera with a weapon in front of it, you cast no
// shadow, and looking down shows you the floor. That is a deliberate omission on Valve's part rather than an
// oversight, but it is not one this engine has to inherit, and the two halves of fixing it are different jobs.
//
// The legs are a separate model, drawn only for the player who owns them. They are a legs-only mesh rather than
// the full body because a full body seen from inside its own head is a mess of neck and shoulder polygons
// clipping through the near plane, and every game that draws first-person legs solves it the same way.
//
// The shadow is the full body, drawn for everyone except its owner but still casting - which is what
// bCastHiddenShadow means. So the shadow on the wall beside you has arms and a head, while the thing you look
// down and see has neither, and there is only ever one of each.
//
// Both play the same sequence, chosen from how the player is moving, so the shadow does what the legs do.

#include "LambdaCharacter.h"

#include "LambdaEngine.h"
#include "LambdaWeapon.h"

#include "Core/LambdaSourceSettings.h"
#include "Formats/SourceMDLFile.h"
#include "Rendering/SourceStudioModelComponent.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"

static bool GDrawFirstPersonLegs = true;
static FAutoConsoleVariableRef CVarDrawFirstPersonLegs(
	TEXT("cl_drawlegs"),
	GDrawFirstPersonLegs,
	TEXT("Draw the player's own legs in first person. Not a Source cvar - Half-Life 2 draws no player body at all."));

static bool GLegsDebug = false;
static FAutoConsoleVariableRef CVarLegsDebug(
	TEXT("cl_legs_debug"),
	GLegsDebug,
	TEXT("Show the legs and body to everyone, and log where they are - for working out why they are not on screen."));

static float GLegsOffsetForward = -24.0f;
static FAutoConsoleVariableRef CVarLegsOffsetForward(
	TEXT("cl_legs_offset_forward"),
	GLegsOffsetForward,
	TEXT("Centimetres to slide the first-person legs along the view axis; negative is backwards."));

static float GLegsOffsetUp = 0.0f;
static FAutoConsoleVariableRef CVarLegsOffsetUp(
	TEXT("cl_legs_offset_up"),
	GLegsOffsetUp,
	TEXT("Centimetres to raise or lower the first-person legs."));

// Source's cl_pitchdown / cl_pitchup, with one non-Source default: straight down is 89 in Half-Life, but at 90
// the player is staring into the open waist of their own legs model, so the floor of the view stops a little
// higher. Up stays at Source's value.
static float GPitchDown = 80.0f;
static FAutoConsoleVariableRef CVarPitchDown(
	TEXT("cl_pitchdown"),
	GPitchDown,
	TEXT("How far below the horizon the view may pitch, in degrees. Source's default is 89."));

static float GPitchUp = 89.0f;
static FAutoConsoleVariableRef CVarPitchUp(
	TEXT("cl_pitchup"),
	GPitchUp,
	TEXT("How far above the horizon the view may pitch, in degrees."));

// The grip: how the weapon world model sits in the shadow's hand, relative to the hand bone's own axes.
static float GWeaponShadowPitch = 0.0f;
static FAutoConsoleVariableRef CVarWeaponShadowPitch(TEXT("cl_weaponshadow_pitch"), GWeaponShadowPitch, TEXT("Grip pitch, degrees."));
static float GWeaponShadowYaw = 90.0f;
static FAutoConsoleVariableRef CVarWeaponShadowYaw(TEXT("cl_weaponshadow_yaw"), GWeaponShadowYaw, TEXT("Grip yaw, degrees."));
static float GWeaponShadowRoll = 0.0f;
static FAutoConsoleVariableRef CVarWeaponShadowRoll(TEXT("cl_weaponshadow_roll"), GWeaponShadowRoll, TEXT("Grip roll, degrees."));

static bool GDrawPlayerShadow = true;
static FAutoConsoleVariableRef CVarDrawPlayerShadow(
	TEXT("cl_drawplayershadow"),
	GDrawPlayerShadow,
	TEXT("Cast a shadow from the player's own body in first person."));

void ALambdaCharacter::SetupPlayerBody()
{
	const ULambdaSourceSettings& Settings = ULambdaSourceSettings::Get();
	const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

	// A Source model stands on its origin, so both meshes hang from the capsule's centre down to the feet.
	if (LegsMesh)
	{
		// Before the model: the cut decides which mesh gets built.
		LegsMesh->SetHiddenBoneSubtree(Settings.PlayerLegsCutBone);
		if (LegsMesh->SetModel(Settings.PlayerLegsModel, GetWorldMaterialLibrary()))
		{
			LegsMesh->SetRelativeLocation(FVector(0.0f, 0.0f, -HalfHeight));
			// Only its owner sees these, and they must not cast - the body behind them is doing that, and two
			// shadows from one player is worse than none.
			LegsMesh->SetOnlyOwnerSee(!GLegsDebug);
			LegsMesh->SetCastShadow(false);
			// Deliberately NOT the view model's first-person pass, though it does clear up the tearing. That
			// pass compresses depth toward the camera by ViewModelFirstPersonScale (0.4), which is right for a
			// gun - it has no world-space truth to keep - and wrong for legs, which have to meet the floor
			// where the player is actually standing. Marking them makes them solid and two and a half times
			// too large, and no offset fixes both at once.
			LegsMesh->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::None);
			UE_LOG(LogLambda, Log, TEXT("player legs: '%s', %d bones, %d sequences"),
				*Settings.PlayerLegsModel, LegsMesh->GetModel()->GetBones().Num(),
				LegsMesh->GetModel()->GetSequences().Num());
		}
		else
		{
			UE_LOG(LogLambda, Warning, TEXT("player legs: '%s' would not load"), *Settings.PlayerLegsModel);
		}
	}

	if (BodyMesh)
	{
		if (BodyMesh->SetModel(Settings.PlayerBodyModel, GetWorldMaterialLibrary()))
		{
			BodyMesh->SetRelativeLocation(FVector(0.0f, 0.0f, -HalfHeight));
			// The point of this one: invisible to the player it belongs to, but its shadow is not.
			BodyMesh->SetOwnerNoSee(!GLegsDebug);
			BodyMesh->SetCastShadow(true);
			BodyMesh->bCastHiddenShadow = true;
			// Its owner never renders it, so the animation LOD would drop it to ten poses a second - and its
			// shadow with it, walking at ten frames a second. The player's own shadow earns the full rate.
			BodyMesh->bAlwaysComposePose = true;

			// The animations were authored empty-handed and swing the arms; a shadow carrying a shotgun with
			// its arms swinging free reads as wrong in exactly the way the real thing would, so the arms are
			// frozen into a two-handed carry, the way Source's bone controllers impose a pose the animation
			// never authored. The rotations are solved here, from this model's own bind skeleton, with the
			// same quaternion code the pose runs through - an external solver kept landing in a subtly
			// different frame, and a solve done inside the frames it will be played back in cannot.
			SolveHoldPose(BodyMesh);

			UE_LOG(LogLambda, Log, TEXT("player body: '%s', %d bones, %d sequences"),
				*Settings.PlayerBodyModel, BodyMesh->GetModel()->GetBones().Num(),
				BodyMesh->GetModel()->GetSequences().Num());
		}
		else
		{
			UE_LOG(LogLambda, Warning, TEXT("player body: '%s' would not load"), *Settings.PlayerBodyModel);
		}
	}
}

FString ALambdaCharacter::WeaponActivitySuffix() const
{
	// The hl2mp animation set is one pose family per weapon carry - ACT_HL2MP_RUN_SHOTGUN and so on - which is
	// the whole reason to use a player model: it already knows how to hold everything. HL2's weapons map onto
	// the families the way hl2mp's acttables map them; anything unknown carries like the SMG, hl2mp's own
	// fallback.
	static const TMap<FString, FString> Suffixes = {
		{ TEXT("weapon_crowbar"),	TEXT("MELEE") },
		{ TEXT("weapon_stunstick"),	TEXT("MELEE") },
		{ TEXT("weapon_pistol"),	TEXT("PISTOL") },
		{ TEXT("weapon_357"),		TEXT("PISTOL") },
		{ TEXT("weapon_smg1"),		TEXT("SMG1") },
		{ TEXT("weapon_ar2"),		TEXT("AR2") },
		{ TEXT("weapon_shotgun"),	TEXT("SHOTGUN") },
		{ TEXT("weapon_crossbow"),	TEXT("CROSSBOW") },
		{ TEXT("weapon_rpg"),		TEXT("RPG") },
		{ TEXT("weapon_frag"),		TEXT("GRENADE") },
		{ TEXT("weapon_slam"),		TEXT("SLAM") },
		{ TEXT("weapon_physcannon"),TEXT("PHYSGUN") },
	};
	if (ActiveWeapon)
	{
		if (const FString* Found = Suffixes.Find(ActiveWeapon->GetWeaponClassName().ToLower()))
		{
			return *Found;
		}
	}
	return TEXT("SMG1");
}

FString ALambdaCharacter::ChoosePlayerBodyActivity() const
{
	// CPlayerAnimState::CalcMainActivity, for the hl2mp set: the same states the label path below knows, named
	// as activities. Direction is not chosen here - these are 9-way blends whose centre is forward movement,
	// and pose parameters are not driven yet.
	const UCharacterMovementComponent* Move = GetCharacterMovement();
	const FString Suffix = WeaponActivitySuffix();
	if (Move && Move->IsFalling())
	{
		return FString::Printf(TEXT("ACT_HL2MP_JUMP_%s"), *Suffix);
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const float Speed = Move ? Move->Velocity.Size2D() / Scale : 0.0f;
	const bool bDucked = bIsCrouched || (Move && Move->bWantsToCrouch);
	const TCHAR* State;
	if (bDucked)
	{
		State = Speed < 10.0f ? TEXT("IDLE_CROUCH") : TEXT("WALK_CROUCH");
	}
	else
	{
		State = Speed < 10.0f ? TEXT("IDLE") : TEXT("RUN");
	}
	return FString::Printf(TEXT("ACT_HL2MP_%s_%s"), State, *Suffix);
}

FString ALambdaCharacter::ChoosePlayerBodySequence() const
{
	// CBasePlayerAnimState::ComputeMainSequence, cut down to what these animations offer: falling beats
	// everything, then ducking, then how fast and which way the feet are actually going.
	const UCharacterMovementComponent* Move = GetCharacterMovement();
	if (!Move)
	{
		return TEXT("idle");
	}
	if (Move->IsFalling())
	{
		return TEXT("fall");
	}

	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const FVector Velocity = Move->Velocity;
	const float Speed = Velocity.Size2D() / Scale;		// units/sec, so the thresholds read as Source's
	// The key, not the finished duck: bIsCrouched only turns true when TIME_TO_DUCK runs out, and a body that
	// waits for it starts crouching four tenths of a second after its player did.
	const bool bDucked = bIsCrouched || Move->bWantsToCrouch;

	if (Speed < 10.0f)
	{
		return bDucked ? TEXT("crouch_idle") : TEXT("idle");
	}

	// Which way the movement points relative to where the body faces, as the four directional animations need.
	const FRotator BodyYaw(0.0f, GetActorRotation().Yaw, 0.0f);
	const FVector Forward = BodyYaw.Vector();
	const FVector Right = FRotationMatrix(BodyYaw).GetUnitAxis(EAxis::Y);
	const FVector Dir = Velocity.GetSafeNormal2D();
	const float Fwd = FVector::DotProduct(Dir, Forward);
	const float Side = FVector::DotProduct(Dir, Right);

	if (bDucked)
	{
		// There is no crouch_backward in this set, so a crouched retreat plays the forward shuffle.
		if (FMath::Abs(Side) > FMath::Abs(Fwd))
		{
			return Side > 0.0f ? TEXT("crouch_strafe_right") : TEXT("crouch_strafe_left");
		}
		return TEXT("crouch_forward");
	}

	// Sprint speed is 320 and walk is 190; anything above the midpoint is a run.
	const TCHAR* Gait = (Speed > 255.0f) ? TEXT("run") : TEXT("walk");
	if (FMath::Abs(Side) > FMath::Abs(Fwd))
	{
		return FString::Printf(TEXT("%s_strafe_%s"), Gait, Side > 0.0f ? TEXT("right") : TEXT("left"));
	}
	return FString::Printf(TEXT("%s_%s"), Gait, Fwd >= 0.0f ? TEXT("forward") : TEXT("backward"));
}

void ALambdaCharacter::UpdatePlayerBody(float DeltaSeconds)
{
	if (!LegsMesh && !BodyMesh)
	{
		return;
	}

	// Loaded on demand rather than at spawn, and reloaded whenever the models have gone: they are built from the
	// map's material library, which does not exist until a map does, and ClearMap drops every built mesh when
	// one is unloaded. Asking each frame costs a null check and means travelling to another map just works.
	if (!LegsMesh->HasModel() && !BodyMesh->HasModel() && GetWorldMaterialLibrary())
	{
		SetupPlayerBody();
	}

	// The view's floor and ceiling (CInput::AdjustPitch clamping to cl_pitchdown/cl_pitchup).
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		if (PC->PlayerCameraManager)
		{
			PC->PlayerCameraManager->ViewPitchMin = -GPitchDown;	// UE pitch is positive upward
			PC->PlayerCameraManager->ViewPitchMax = GPitchUp;
		}
	}

	// The body faces where the player is looking, not where the pawn happens to be pointing. Source twists the
	// spine to split the two apart; with these animations the whole body turns, which reads correctly from
	// inside the head and from the shadow's point of view alike.
	const FRotator Facing(0.0f, GetControlRotation().Yaw, 0.0f);
	SetActorRotation(Facing);

	// The capsule shrinks when ducking and the models hang from its centre, so the offset has to follow it or
	// the feet sink through the floor mid-crouch.
	const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	FVector Offset(0.0f, 0.0f, -HalfHeight);
	if (GLegsDebug)
	{
		// Stood out in front where they can be looked at: from inside the hips, a leg and a broken leg look
		// much the same.
		Offset += FVector(150.0f, 0.0f, 0.0f);
	}

	const FString WantedActivity = ChoosePlayerBodyActivity();
	const FString Wanted = ChoosePlayerBodySequence();
	const bool bMoving = GetCharacterMovement() && GetCharacterMovement()->Velocity.Size2D() > 1.0f;

	USourceStudioModelComponent* Meshes[] = { LegsMesh, BodyMesh };
	const bool bWanted[] = { GDrawFirstPersonLegs, GDrawPlayerShadow };
	for (int32 i = 0; i < 2; ++i)
	{
		USourceStudioModelComponent* Part = Meshes[i];
		if (!Part || !Part->HasModel())
		{
			continue;
		}
		Part->SetVisibility(bWanted[i]);
		if (!bWanted[i])
		{
			continue;
		}
		// The legs alone are nudged along the view axis. They hang directly under the eye otherwise, so looking
		// down lands on the tops of the knees rather than along the thighs - a real head sits forward of the
		// hips and this makes up the difference. The shadow body is not moved: it has to stay where the player
		// actually is, or the shadow steps away from the feet.
		Part->SetRelativeLocation(i == 0 ? Offset + FVector(GLegsOffsetForward, 0.0f, GLegsOffsetUp) : Offset);

		// A player model answers to activities (the hl2mp set); anything else falls back to sequence labels
		// (the gordon models' walk_forward and friends).
		int32 Sequence = Part->GetModel()->SelectWeightedSequence(WantedActivity);
		if (Sequence == INDEX_NONE)
		{
			Sequence = Part->GetModel()->FindSequenceByLabel(Wanted);
		}
		if (Sequence != INDEX_NONE && Sequence != Part->GetSequence())
		{
			Part->PlaySequence(Sequence);
		}

		// The cycle keeps pace with the ground so the feet do not skate. Source reads the authored speed out of
		// the sequence's root motion (GetSequenceGroundSpeed); these animations are in place and carry none, so
		// the authored speed falls back to the speed of the gait the sequence is named for - a walk cycle is
		// authored to look right at walking speed, a crouch shuffle at crouch speed.
		float Rate = 1.0f;
		if (bMoving)
		{
			float Authored = Part->GetSequenceGroundSpeed();
			if (Authored <= 1.0f)
			{
				if (Wanted.StartsWith(TEXT("run"))) { Authored = 320.0f; }
				else if (Wanted.StartsWith(TEXT("crouch"))) { Authored = 190.0f * 0.33f; }
				else if (Wanted.StartsWith(TEXT("walk"))) { Authored = 190.0f; }
			}
			if (Authored > 1.0f)
			{
				const float Scale = ULambdaSourceSettings::Get().UnitScale;
				Rate = FMath::Clamp((GetCharacterMovement()->Velocity.Size2D() / Scale) / Authored, 0.25f, 3.0f);
			}
		}
		Part->SetPlaybackRate(Rate);

		if (i == 1)
		{
			UpdateWeaponShadow(Part);
		}

		if (GLegsDebug && i == 0)
		{
			static float NextLog = 0.0f;
			const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
			if (Now > NextLog)
			{
				NextLog = Now + 2.0f;
				const FBoxSphereBounds B = Part->Bounds;
				UE_LOG(LogLambda, Display, TEXT("legs: origin %s extent %s | camera %s | seq %d '%s' vis=%d"),
					*B.Origin.ToCompactString(), *B.BoxExtent.ToCompactString(),
					*FirstPersonCamera->GetComponentLocation().ToCompactString(),
					Part->GetSequence(), *Wanted, Part->IsVisible() ? 1 : 0);
			}
		}
	}
}

void ALambdaCharacter::SolveHoldPose(USourceStudioModelComponent* Body)
{
	// A low two-handed carry: right hand at the grip by the hip, left hand out on the forend. Aim constraints
	// rather than frozen rotations, because a frozen elbow still rides wherever the animated shoulder above it
	// swings - the constraint re-solves inside every composed pose, so the carry holds through the walk cycle.
	// Directions are model space: +x the way the model faces, +y its left, +z up.
	Body->SetBoneAimConstraint(TEXT("mixamorig:RightArm"), TEXT("mixamorig:RightForeArm"), FVector3f(0.30f, -0.18f, -0.94f));
	Body->SetBoneAimConstraint(TEXT("mixamorig:RightForeArm"), TEXT("mixamorig:RightHand"), FVector3f(0.90f, 0.25f, 0.15f));
	Body->SetBoneAimConstraint(TEXT("mixamorig:LeftArm"), TEXT("mixamorig:LeftForeArm"), FVector3f(0.40f, 0.10f, -0.91f));
	Body->SetBoneAimConstraint(TEXT("mixamorig:LeftForeArm"), TEXT("mixamorig:LeftHand"), FVector3f(0.80f, -0.50f, 0.15f));
}

void ALambdaCharacter::UpdateWeaponShadow(USourceStudioModelComponent* Body)
{
	// The shadow carries what the player carries. The first-person view model is arms and a gun floating at
	// the camera and casts nothing; the thing on the wall should still be holding something, so the weapon's
	// *world* model - the one Source shows in everyone else's hands - rides the shadow body's right hand.
	if (!WeaponShadowMesh)
	{
		return;
	}
	const FString WantedClass = ActiveWeapon ? ActiveWeapon->GetWeaponClassName() : FString();
	if (WantedClass != WeaponShadowClass)
	{
		WeaponShadowClass = WantedClass;
		WeaponShadowBone = -1;
		const FSourceWeaponInfo* Info = ActiveWeapon ? &ActiveWeapon->GetWeaponInfo() : nullptr;
		if (Info && !Info->PlayerModel.IsEmpty() && WeaponShadowMesh->SetModel(Info->PlayerModel, GetWorldMaterialLibrary()))
		{
			WeaponShadowMesh->SetOwnerNoSee(true);
			WeaponShadowMesh->SetCastShadow(true);
			WeaponShadowMesh->bCastHiddenShadow = true;
			// Find the hand once per body model: the rig is Mixamo-named.
			const TArray<FSourceStudioBone>& Bones = Body->GetModel()->GetBones();
			for (int32 b = 0; b < Bones.Num(); ++b)
			{
				// ValveBiped's right hand, or a Mixamo rig's - whichever this model is built on.
				if (Bones[b].Name.Equals(TEXT("ValveBiped.Bip01_R_Hand"), ESearchCase::IgnoreCase)
					|| Bones[b].Name.EndsWith(TEXT("RightHand")))
				{
					WeaponShadowBone = b;
					break;
				}
			}
			UE_LOG(LogLambda, Log, TEXT("weapon shadow: '%s' in hand bone %d"), *Info->PlayerModel, WeaponShadowBone);
		}
		else
		{
			WeaponShadowMesh->ClearModel();
		}
	}

	const bool bShow = WeaponShadowMesh->HasModel() && WeaponShadowBone >= 0 && Body->IsVisible();
	WeaponShadowMesh->SetVisibility(bShow);
	// In debug the body shows itself to its owner, so the weapon in its hand has to as well.
	if (WeaponShadowMesh->bOwnerNoSee == GLegsDebug)
	{
		WeaponShadowMesh->SetOwnerNoSee(!GLegsDebug);
	}
	if (bShow)
	{
		// The hand's own transform, with a fixed grip offset. With a real player animation set the hands are
		// posed holding a weapon, so the gun follows the hand the way Source's bonemerge would - the offset is
		// the difference between the w_ model's origin and where a palm actually grips, found by eye once.
		const FTransform Hand = Body->GetBoneWorldTransform(WeaponShadowBone);
		const FQuat Grip = FRotator(GWeaponShadowPitch, GWeaponShadowYaw, GWeaponShadowRoll).Quaternion();
		WeaponShadowMesh->SetWorldLocationAndRotation(Hand.GetLocation(), Hand.GetRotation() * Grip);
		return;
	}

}
