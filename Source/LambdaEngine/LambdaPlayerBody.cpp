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

static float GLegsOffsetForward = -14.0f;
static FAutoConsoleVariableRef CVarLegsOffsetForward(
	TEXT("cl_legs_offset_forward"),
	GLegsOffsetForward,
	TEXT("Centimetres to slide the first-person legs along the view axis; negative is backwards."));

static float GLegsOffsetUp = 0.0f;
static FAutoConsoleVariableRef CVarLegsOffsetUp(
	TEXT("cl_legs_offset_up"),
	GLegsOffsetUp,
	TEXT("Centimetres to raise or lower the first-person legs."));

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
	const bool bDucked = bIsCrouched;

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

		const int32 Sequence = Part->GetModel()->FindSequenceByLabel(Wanted);
		if (Sequence != INDEX_NONE && Sequence != Part->GetSequence())
		{
			Part->PlaySequence(Sequence);
		}

		// The walk cycle keeps pace with the ground rather than running at whatever rate it was authored at,
		// so the feet do not skate (CBaseAnimating::GetSequenceGroundSpeed drives the same thing in Source).
		float Rate = 1.0f;
		if (bMoving)
		{
			const float Authored = Part->GetSequenceGroundSpeed();
			if (Authored > 1.0f)
			{
				const float Scale = ULambdaSourceSettings::Get().UnitScale;
				Rate = FMath::Clamp((GetCharacterMovement()->Velocity.Size2D() / Scale) / Authored, 0.25f, 3.0f);
			}
		}
		Part->SetPlaybackRate(Rate);

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
