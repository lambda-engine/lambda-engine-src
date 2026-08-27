#include "LambdaWeapon.h"
#include "LambdaCharacter.h"
#include "Creatures/SourceNPCBase.h"
#include "Rendering/SourceStudioModelComponent.h"
#include "LambdaEngine.h"
#include "Audio/LambdaSoundLibrary.h"
#include "Core/LambdaSourceSettings.h"
#include "Weapons/SourceAmmoDef.h"
#include "Gameplay/SourceDamage.h"
#include "GameFramework/DamageType.h"
#include "Core/SourceCoordinates.h"
#include "Rendering/SourceImpactEffects.h"
#include "Camera/CameraComponent.h"
#include "Engine/HitResult.h"
#include "Engine/DamageEvents.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"

// ---------------------------------------------------------------------------------------------------------------------
// ALambdaWeapon - CBaseCombatWeapon
// ---------------------------------------------------------------------------------------------------------------------

ALambdaWeapon::ALambdaWeapon()
{
	PrimaryActorTick.bCanEverTick = false;	// driven from the owning character's tick, like ItemPostFrame
}

void ALambdaWeapon::InitializeFromScript(const FString& InClassName)
{
	if (const FSourceWeaponInfo* Info = FSourceWeaponScripts::Get().Find(InClassName))
	{
		WeaponInfo = *Info;
	}
	else
	{
		UE_LOG(LogLambda, Warning, TEXT("No weapon script for '%s'"), *InClassName);
		WeaponInfo = FSourceWeaponInfo();
		WeaponInfo.ClassName = InClassName;
	}

	// CBaseCombatWeapon::Spawn gives the weapon a full clip.
	Clip1 = WeaponInfo.ClipSize;

	// CBaseCombatWeapon::Precache -> PrecacheScriptSound for every SoundData entry: decode them now rather than
	// on the first shot.
	for (const auto& Pair : WeaponInfo.Sounds)
	{
		float Volume, Pitch;
		FLambdaSoundCache::Get().CreateWaveResolved(this, Pair.Value, false, Volume, Pitch);
	}

	UE_LOG(LogLambda, Log, TEXT("Weapon '%s': clip %d/%d, ammo '%s', single_shot '%s'"),
		*WeaponInfo.ClassName, Clip1, WeaponInfo.ClipSize, *WeaponInfo.PrimaryAmmo, *WeaponInfo.GetSound(TEXT("single_shot")));
}

float ALambdaWeapon::GetCurrentTime() const
{
	return GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
}

void ALambdaWeapon::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
}

void ALambdaWeapon::WeaponSound(ESourceWeaponSound Sound)
{
	// The SoundData keys used by the weapon scripts.
	const TCHAR* Key = TEXT("");
	switch (Sound)
	{
	case ESourceWeaponSound::Empty:		Key = TEXT("empty"); break;
	case ESourceWeaponSound::Single:	Key = TEXT("single_shot"); break;
	case ESourceWeaponSound::Double:	Key = TEXT("double_shot"); break;
	case ESourceWeaponSound::Burst:		Key = TEXT("burst"); break;
	case ESourceWeaponSound::Reload:	Key = TEXT("reload"); break;
	case ESourceWeaponSound::Special1:	Key = TEXT("special1"); break;
	case ESourceWeaponSound::Special2:	Key = TEXT("special2"); break;
	case ESourceWeaponSound::Melee_Miss:		Key = TEXT("melee_miss"); break;
	case ESourceWeaponSound::Melee_Hit:			Key = TEXT("melee_hit"); break;
	case ESourceWeaponSound::Melee_HitWorld:	Key = TEXT("melee_hit_world"); break;
	default: break;
	}

	const FString SoundName = WeaponInfo.GetSound(Key);
	if (SoundName.IsEmpty())
	{
		return;
	}

	float Volume = 1.0f, Pitch = 1.0f;
	if (ULambdaSoundWave* Wave = FLambdaSoundCache::Get().CreateWaveResolved(this, SoundName, false, Volume, Pitch))
	{
		// Weapon sounds come from the player, so play them 2D-ish at the owner's location.
		const FVector Location = OwningCharacter.IsValid() ? OwningCharacter->GetActorLocation() : GetActorLocation();
		UGameplayStatics::SpawnSoundAtLocation(this, Wave, Location, FRotator::ZeroRotator, Volume, Pitch);
	}
}

void ALambdaWeapon::HandleFireOnEmpty()
{
	// CBaseCombatWeapon::HandleFireOnEmpty - click, and don't spam it.
	if (NextEmptySoundTime < GetCurrentTime())
	{
		WeaponSound(ESourceWeaponSound::Empty);
		NextEmptySoundTime = GetCurrentTime() + 0.5f;
	}
	NextPrimaryAttack = GetCurrentTime() + 0.5f;
}

bool ALambdaWeapon::SendWeaponAnim(const FString& ActivityName)
{
	// CBaseCombatWeapon::SendWeaponAnim -> SetIdealActivity: play the sequence, then set the time at which the
	// weapon falls back to its idle animation.
	ALambdaCharacter* WeaponOwner = OwningCharacter.Get();
	if (!WeaponOwner)
	{
		return false;
	}
	// The third-person half of the same moment: the shadow body plays its attack or reload gesture. Keyed off
	// the view model activity because every weapon's fire and reload funnels through here already.
	//
	// A swing is an attack too. Bludgeon weapons never say PRIMARYATTACK - CBaseHLBludgeonWeapon sends
	// ACT_VM_HITCENTER or ACT_VM_MISSCENTER depending on whether it connected - so matching only the shooting
	// activities left the crowbar swinging in first person while the shadow stood still.
	auto IsAttack = [](const FString& Activity)
	{
		static const TCHAR* Attacks[] = {
			TEXT("PRIMARYATTACK"), TEXT("SECONDARYATTACK"),
			TEXT("HITCENTER"), TEXT("HITLEFT"), TEXT("HITRIGHT"), TEXT("HITSLICE"),
			TEXT("MISSCENTER"), TEXT("MISSLEFT"), TEXT("MISSRIGHT"), TEXT("MISSSLICE"),
			TEXT("SWINGHARD"), TEXT("SWINGHIT"), TEXT("SWINGMISS"),
		};
		for (const TCHAR* Attack : Attacks)
		{
			if (Activity.Contains(Attack))
			{
				return true;
			}
		}
		return false;
	};
	if (IsAttack(ActivityName))
	{
		WeaponOwner->OnWeaponAttackAnim();
	}
	else if (ActivityName.Contains(TEXT("RELOAD")))
	{
		WeaponOwner->OnWeaponReloadAnim();
	}
	if (!WeaponOwner->SendViewModelAnim(ActivityName))
	{
		// The model has no sequence for that activity. Not fatal - the weapon keeps whatever it was playing -
		// but it means a weapon is asking for an animation its view model has not got, which is worth saying.
		UE_LOG(LogLambda, Verbose, TEXT("%s: view model has no activity '%s'"), *WeaponInfo.ClassName, *ActivityName);
		return false;
	}
	SetWeaponIdleTime(GetCurrentTime() + SequenceDuration());
	return true;
}

float ALambdaWeapon::SequenceDuration() const
{
	const ALambdaCharacter* WeaponOwner = OwningCharacter.Get();
	const USourceStudioModelComponent* ViewModel = WeaponOwner ? WeaponOwner->GetViewModelMesh() : nullptr;
	return ViewModel ? ViewModel->GetSequenceDuration() : 0.0f;
}

void ALambdaWeapon::WeaponIdle()
{
	// CBaseCombatWeapon::WeaponIdle
	if (HasWeaponIdleTimeElapsed())
	{
		SendWeaponAnim(TEXT("ACT_VM_IDLE"));
	}
}

void ALambdaWeapon::Holster()
{
	bHolstered = true;
	// The held weapon is the view model; hiding it is what "putting it away" looks like from the player's side.
	if (ALambdaCharacter* WeaponOwner = OwningCharacter.Get())
	{
		if (USourceStudioModelComponent* ViewModel = WeaponOwner->GetViewModelMesh())
		{
			ViewModel->SetVisibility(false, true);
		}
	}
	// Nothing is being held down any more by the time it comes back out.
	bAttackHeld = false;
	bAttackPressedThisFrame = false;
	bAttack2Held = false;
	bAttack2PressedThisFrame = false;
	bReloadHeld = false;
	bInReload = false;
}

void ALambdaWeapon::Deploy()
{
	bHolstered = false;
	if (ALambdaCharacter* WeaponOwner = OwningCharacter.Get())
	{
		if (USourceStudioModelComponent* ViewModel = WeaponOwner->GetViewModelMesh())
		{
			ViewModel->SetVisibility(true, true);
		}
	}
	// Deploy plays the draw animation and holds fire until it has run.
	SendWeaponAnim(TEXT("ACT_VM_DRAW"));
	NextPrimaryAttack = GetCurrentTime() + 0.5f;
	SetWeaponIdleTime(GetCurrentTime() + 0.5f);
}

void ALambdaWeapon::ItemPostFrame()
{
	// CBaseCombatWeapon::ItemPostFrame
	ALambdaCharacter* WeaponOwner = OwningCharacter.Get();
	if (!WeaponOwner || bHolstered)
	{
		return;
	}

	const float Now = GetCurrentTime();

	// m_fFireDuration: how long the trigger has been down. The machine guns read it to work out how far the
	// view has climbed, so it has to be kept whether or not this weapon cares.
	if (bAttackHeld)
	{
		FireDuration += GetWorld() ? GetWorld()->GetDeltaSeconds() : 0.0f;
	}
	else
	{
		FireDuration = 0.0f;
	}

	// Finish a reload that is in flight (Source drives this off the view-model animation).
	if (bInReload && Now >= ReloadFinishTime)
	{
		const int32 Wanted = WeaponInfo.ClipSize - Clip1;
		const int32 Available = WeaponOwner->GetAmmoCount(WeaponInfo.PrimaryAmmo);
		const int32 Taken = FMath::Min(Wanted, Available);
		Clip1 += Taken;
		WeaponOwner->RemoveAmmo(WeaponInfo.PrimaryAmmo, Taken);
		bInReload = false;
	}

	if (bInReload)
	{
		return;
	}

	if (bAttackHeld && NextPrimaryAttack <= Now)
	{
		// Clip empty? Or out of ammo on a no-clip weapon?
		if ((UsesClipsForAmmo1() && Clip1 <= 0) ||
			(!UsesClipsForAmmo1() && WeaponOwner->GetAmmoCount(WeaponInfo.PrimaryAmmo) <= 0))
		{
			HandleFireOnEmpty();
		}
		else
		{
			// If the firing button was just pressed, reset the firing time.
			if (bAttackPressedThisFrame)
			{
				NextPrimaryAttack = Now;
			}
			PrimaryAttack();
		}
	}

	if (bAttack2Held && NextSecondaryAttack <= Now)
	{
		SecondaryAttack();
	}

	// Reload pressed
	if (bReloadHeld && UsesClipsForAmmo1() && !bInReload)
	{
		Reload();
	}

	// No buttons down: let the weapon drift back to its idle animation.
	if (!bAttackHeld && !bAttack2Held && !bReloadHeld && !bInReload)
	{
		WeaponIdle();
	}

	bAttackPressedThisFrame = false;
	bAttack2PressedThisFrame = false;
}

void ALambdaWeapon::PrimaryAttack()
{
	// CBaseCombatWeapon::PrimaryAttack
	ALambdaCharacter* WeaponOwner = OwningCharacter.Get();
	if (!WeaponOwner)
	{
		return;
	}

	// If my clip is empty (and I use clips) start reload
	if (UsesClipsForAmmo1() && !Clip1)
	{
		Reload();
		return;
	}

	SendWeaponAnim(GetPrimaryAttackActivity());
	WeaponOwner->DoMuzzleFlash();

	const float FireRate = GetFireRate();
	const float Now = GetCurrentTime();

	// Fire as many shots as the elapsed time allows, so the rate is framerate independent.
	int32 Shots = 0;
	while (NextPrimaryAttack <= Now)
	{
		WeaponSound(ESourceWeaponSound::Single);
		NextPrimaryAttack = NextPrimaryAttack + FireRate;
		++Shots;
		if (FireRate <= 0.0f)
		{
			break;
		}
	}

	if (UsesClipsForAmmo1())
	{
		Shots = FMath::Min(Shots, Clip1);
		Clip1 -= Shots;
	}
	else
	{
		Shots = FMath::Min(Shots, WeaponOwner->GetAmmoCount(WeaponInfo.PrimaryAmmo));
		WeaponOwner->RemoveAmmo(WeaponInfo.PrimaryAmmo, Shots);
	}

	// Damage comes from the ammo type's skill cvar (sk_plr_dmg_pistol and friends).
	float Damage = 0.0f;
	if (const FSourceAmmoType* Ammo = FSourceAmmoDef::Get().Find(WeaponInfo.PrimaryAmmo))
	{
		Damage = Ammo->PlayerDamage;
	}

	const FVector Spread = GetBulletSpread();
	for (int32 i = 0; i < Shots; ++i)
	{
		FireBullet(Damage, Spread);
	}
}

void ALambdaWeaponCrowbar::InitializeFromScript(const FString& InClassName)
{
	Super::InitializeFromScript(InClassName);
	DamagePerSwing = FSourceAmmoDef::Get().GetSkillValue(TEXT("sk_plr_dmg_crowbar"), 10.0f);
}

void ALambdaWeaponCrowbar::ItemPostFrame()
{
	// CBaseHLBludgeonWeapon::ItemPostFrame: swing while the attack is held, idle otherwise.
	ALambdaCharacter* WeaponOwner = OwningCharacter.Get();
	if (!WeaponOwner || IsHolstered())
	{
		return;
	}
	const float Now = GetCurrentTime();
	if (bAttackHeld && Now >= NextPrimaryAttack)
	{
		Swing();
		NextPrimaryAttack = Now + GetFireRate();
		SetWeaponIdleTime(Now + 1.0f);
	}
	else if (HasWeaponIdleTimeElapsed())
	{
		WeaponIdle();
	}
	bAttackPressedThisFrame = false;
}

void ALambdaWeaponCrowbar::Swing()
{
	ALambdaCharacter* WeaponOwner = OwningCharacter.Get();
	UWorld* World = GetWorld();
	if (!WeaponOwner || !World)
	{
		return;
	}
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	constexpr float CROWBAR_RANGE = 75.0f;
	constexpr float BLUDGEON_HULL_DIM = 16.0f;

	FVector Start;
	FRotator EyeRot;
	WeaponOwner->GetActorEyesViewPoint(Start, EyeRot);
	const FVector Dir = EyeRot.Vector();
	const FVector End = Start + Dir * CROWBAR_RANGE * Scale;

	// "Try a ray" - and unlike a bullet, a bludgeon hits an NPC's hull, not its hitboxes.
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(LambdaCrowbar), /*bTraceComplex=*/ true, WeaponOwner);
	Params.bReturnFaceIndex = true;
	bool bHit = World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);

	if (!bHit)
	{
		// Back off by the hull's "radius" and sweep the bludgeon hull, taking the target only if the player is
		// sort of facing it (the 0.707 dot in Swing).
		const FVector HullEnd = End - Dir * (1.732f * BLUDGEON_HULL_DIM * Scale);
		FHitResult HullHit;
		if (World->SweepSingleByChannel(HullHit, Start, HullEnd, FQuat::Identity, ECC_Visibility,
			FCollisionShape::MakeBox(FVector(BLUDGEON_HULL_DIM * Scale)), Params) && HullHit.GetActor())
		{
			const FVector ToTarget = (HullHit.GetActor()->GetActorLocation() - Start).GetSafeNormal();
			if (FVector::DotProduct(ToTarget, Dir) >= 0.70721f)
			{
				Hit = HullHit;
				bHit = true;
			}
		}
	}

	// The swing whoosh plays either way; the animation says whether it landed.
	WeaponSound(ESourceWeaponSound::Single);
	SendWeaponAnim(bHit ? TEXT("ACT_VM_HITCENTER") : TEXT("ACT_VM_MISSCENTER"));

	if (!bHit)
	{
		return;
	}

	// CBaseHLBludgeonWeapon::Hit: the view kick, the damage with a melee shove behind it, and the impact effect.
	WeaponOwner->ViewPunch(FRotator(-FMath::FRandRange(1.0f, 2.0f), FMath::FRandRange(-2.0f, -1.0f), 0.0f));

	if (AActor* HitActor = Hit.GetActor())
	{
		const bool bFlesh = Cast<ASourceNPCBase>(HitActor) != nullptr;
		WeaponSound(bFlesh ? ESourceWeaponSound::Melee_Hit : ESourceWeaponSound::Melee_HitWorld);

		// CalculateMeleeDamageForce: the blow shoves what it lands on along the swing.
		const FVector Force = Dir * DamagePerSwing * 500.0f * Scale;	// kg*cm/s
		FSourceDamageEvent DamageEvent(DamagePerSwing, Hit, Dir, UDamageType::StaticClass(), Force,
			SourceDamageType::DMG_CLUB, SourceHitGroup::HITGROUP_GENERIC);
		HitActor->TakeDamage(DamagePerSwing, DamageEvent, WeaponOwner->GetController(), WeaponOwner);
	}

	// UTIL_ImpactTrace on the world: decal, dust and the surface's own impact noise (skipped on flesh - the
	// melee_hit sound carries that).
	if (!Cast<ASourceNPCBase>(Hit.GetActor()))
	{
		SourceImpact::PlayImpact(Hit, WeaponOwner->GetWorldMaterialLibrary(), this, Dir, DamagePerSwing);
	}
}

void ALambdaWeapon::FireBullet(float Damage, const FVector& Spread)
{
	ALambdaCharacter* WeaponOwner = OwningCharacter.Get();
	UWorld* World = GetWorld();
	if (!WeaponOwner || !World)
	{
		return;
	}

	FVector EyeLocation;
	FRotator EyeRotation;
	WeaponOwner->GetActorEyesViewPoint(EyeLocation, EyeRotation);

	// Source's spread is a cone expressed as the sine of the half-angle on each axis; it perturbs the shot in the
	// shooter's right/up plane (CBaseEntity::FireBullets).
	FVector Dir = EyeRotation.Vector();
	if (!Spread.IsNearlyZero())
	{
		const FVector Right = FRotationMatrix(EyeRotation).GetUnitAxis(EAxis::Y);
		const FVector Up = FRotationMatrix(EyeRotation).GetUnitAxis(EAxis::Z);
		const float X = FMath::FRandRange(-0.5f, 0.5f) + FMath::FRandRange(-0.5f, 0.5f);
		const float Y = FMath::FRandRange(-0.5f, 0.5f) + FMath::FRandRange(-0.5f, 0.5f);
		Dir = Dir + Right * (X * Spread.X) + Up * (Y * Spread.Y);
		Dir.Normalize();
	}

	// MAX_TRACE_LENGTH is 56756 units in Source.
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const FVector End = EyeLocation + Dir * 56756.0f * Scale;

	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(LambdaFireBullet), /*bTraceComplex=*/ true, WeaponOwner);
	// The face index is what resolves the impact point back to a material, and from there to a $surfaceprop.
	Params.bReturnFaceIndex = true;
	int32 HitGroup = SourceHitGroup::HITGROUP_GENERIC;
	if (SourceImpact::TraceBullet(World, EyeLocation, End, Params, Hit, HitGroup))
	{
		UE_LOG(LogLambda, Verbose, TEXT("bullet hit %s at %s (damage %g)"),
			*GetNameSafe(Hit.GetActor()), *Hit.ImpactPoint.ToString(), Damage);

		// UTIL_ImpactTrace: the decal and impact sound come from the surface that was struck.
		SourceImpact::PlayImpact(Hit, WeaponOwner->GetWorldMaterialLibrary(), this, Dir, Damage);

		// DispatchTraceAttack with CalculateBulletDamageForce: the bullet's impulse (kg*in/s from the ammo table,
		// phys_pushscale 1) along its travel direction, which is what a ragdoll is kicked with.
		if (AActor* HitActor = Hit.GetActor())
		{
			float ImpulseInPerSec = 0.0f;
			if (const FSourceAmmoType* Ammo = FSourceAmmoDef::Get().Find(WeaponInfo.PrimaryAmmo))
			{
				ImpulseInPerSec = Ammo->DamageForce;
			}
			const FVector Force = Dir * ImpulseInPerSec * 2.54f;	// kg*cm/s
			FSourceDamageEvent DamageEvent(Damage, Hit, Dir, UDamageType::StaticClass(), Force, SourceDamageType::DMG_BULLET, HitGroup);
			HitActor->TakeDamage(Damage, DamageEvent, WeaponOwner->GetController(), WeaponOwner);
		}
	}
}

bool ALambdaWeapon::Reload()
{
	// CBaseCombatWeapon::Reload / DefaultReload
	ALambdaCharacter* WeaponOwner = OwningCharacter.Get();
	if (!WeaponOwner || bInReload)
	{
		return false;
	}
	if (Clip1 >= WeaponInfo.ClipSize)
	{
		return false;
	}
	if (WeaponOwner->GetAmmoCount(WeaponInfo.PrimaryAmmo) <= 0)
	{
		return false;
	}

	// CBaseCombatWeapon::DefaultReload: the reload takes exactly as long as its view-model animation.
	WeaponSound(ESourceWeaponSound::Reload);
	SendWeaponAnim(TEXT("ACT_VM_RELOAD"));
	bInReload = true;

	const float Duration = SequenceDuration();
	ReloadFinishTime = GetCurrentTime() + (Duration > 0.0f ? Duration : 1.5f);
	NextPrimaryAttack = ReloadFinishTime;
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// ALambdaWeaponPistol - CWeaponPistol
// ---------------------------------------------------------------------------------------------------------------------

ALambdaWeaponPistol::ALambdaWeaponPistol()
{
}

FVector ALambdaWeaponPistol::GetBulletSpread() const
{
	// CWeaponPistol::GetBulletSpread with pistol_use_new_accuracy 1: lerp from VECTOR_CONE_1DEGREES to
	// VECTOR_CONE_6DEGREES as the accuracy penalty builds up.
	static const FVector Cone1Degree(0.00873f, 0.00873f, 0.00873f);
	static const FVector Cone6Degrees(0.05234f, 0.05234f, 0.05234f);

	const float Ramp = FMath::Clamp(AccuracyPenalty / PISTOL_ACCURACY_MAXIMUM_PENALTY_TIME, 0.0f, 1.0f);
	return FMath::Lerp(Cone1Degree, Cone6Degrees, Ramp);
}

void ALambdaWeaponPistol::PrimaryAttack()
{
	// CWeaponPistol::PrimaryAttack
	const float Now = GetCurrentTime();

	if ((Now - LastAttackTime) > 0.5f)
	{
		NumShotsFired = 0;
	}
	else
	{
		NumShotsFired++;
	}

	LastAttackTime = Now;
	SoonestPrimaryAttack = Now + PISTOL_FASTEST_REFIRE_TIME;

	Super::PrimaryAttack();

	// Add an accuracy penalty which can move past our maximum penalty time if we're really spastic
	AccuracyPenalty += PISTOL_ACCURACY_SHOT_PENALTY_TIME;
}

void ALambdaWeaponPistol::DryFire()
{
	// CWeaponPistol::DryFire
	WeaponSound(ESourceWeaponSound::Empty);
	SendWeaponAnim(TEXT("ACT_VM_DRYFIRE"));
	SoonestPrimaryAttack = GetCurrentTime() + PISTOL_FASTEST_DRY_REFIRE_TIME;
	NextPrimaryAttack = GetCurrentTime() + PISTOL_FASTEST_DRY_REFIRE_TIME;
}

void ALambdaWeaponPistol::ItemPostFrame()
{
	// CWeaponPistol::ItemPostFrame
	Super::ItemPostFrame();

	if (bInReload)
	{
		return;
	}

	const float Now = GetCurrentTime();

	// The pistol decays its accuracy penalty over time (CWeaponPistol::ItemPreFrame does this in Source).
	AccuracyPenalty = FMath::Max(0.0f, AccuracyPenalty - GetWorld()->GetDeltaSeconds());

	// Allow a refire as fast as the player can click
	if (!bAttackHeld && SoonestPrimaryAttack < Now)
	{
		NextPrimaryAttack = Now - 0.1f;
	}
	else if (bAttackHeld && NextPrimaryAttack < Now && Clip1 <= 0)
	{
		DryFire();
	}
}

// =====================================================================================================================
// weapon_smg1 - CWeaponSMG1 on CHLMachineGun
// =====================================================================================================================

FVector ALambdaWeaponSMG1::GetBulletSpread() const
{
	// VECTOR_CONE_5DEGREES (basecombatweapon_shared.h)
	return FVector(0.04362f, 0.04362f, 0.04362f);
}

FString ALambdaWeaponSMG1::GetPrimaryAttackActivity() const
{
	// CWeaponSMG1::GetPrimaryAttackActivity: the first shots walk up a ladder of recoil animations so a burst
	// does not play the same frame over and over.
	if (NumShotsFired < 2)
	{
		return TEXT("ACT_VM_PRIMARYATTACK");
	}
	if (NumShotsFired < 3)
	{
		return TEXT("ACT_VM_RECOIL1");
	}
	if (NumShotsFired < 4)
	{
		return TEXT("ACT_VM_RECOIL2");
	}
	return TEXT("ACT_VM_RECOIL3");
}

void ALambdaWeaponSMG1::PrimaryAttack()
{
	// CHLMachineGun::PrimaryAttack counts the shot before it picks the animation, which is what makes the ladder
	// above advance. The firing itself - one shot per fire-rate interval elapsed, so the rate does not depend on
	// the frame rate - is the base class's.
	++NumShotsFired;
	Super::PrimaryAttack();
	AddViewKick();
}

void ALambdaWeaponSMG1::AddViewKick()
{
	// CHLMachineGun::DoMachineGunKick. The kick grows with how long the trigger has been held rather than with
	// the number of rounds gone, and stops growing at SLIDE_LIMIT.
	ALambdaCharacter* WeaponOwner = GetOwningCharacter();
	if (!WeaponOwner)
	{
		return;
	}

	constexpr float KICK_MIN_X = 0.2f;	// degrees
	constexpr float KICK_MIN_Y = 0.2f;
	constexpr float KICK_MIN_Z = 0.1f;

	const float Duration = FMath::Min(FireDuration, SLIDE_LIMIT);
	const float KickPerc = SLIDE_LIMIT > 0.0f ? Duration / SLIDE_LIMIT : 0.0f;

	FVector Scratch;
	Scratch.X = -(KICK_MIN_X + (MAX_VERTICAL_KICK * KickPerc));
	Scratch.Y = -(KICK_MIN_Y + (MAX_VERTICAL_KICK * KickPerc)) / 3.0f;
	Scratch.Z = KICK_MIN_Z + (MAX_VERTICAL_KICK * KickPerc) / 8.0f;

	// Wibble left and right, wobble up and down.
	if (FMath::RandRange(-1, 1) >= 0)
	{
		Scratch.Y *= -1.0f;
	}
	if (FMath::RandRange(-1, 1) >= 0)
	{
		Scratch.Z *= -1.0f;
	}

	// UTIL_ClipPunchAngleOffset holds the total punch inside QAngle(24, 3, 1).
	// NOTE from the original: the 0.5 is tuned to match the old effect from before the punch was simulated.
	WeaponOwner->ViewPunch(FRotator(Scratch.X * 0.5f, Scratch.Y * 0.5f, Scratch.Z * 0.5f));
}

void ALambdaWeaponSMG1::ItemPostFrame()
{
	// CHLMachineGun::ItemPostFrame: let go of the trigger and the recoil ladder starts again from the bottom.
	if (!bAttackHeld)
	{
		NumShotsFired = 0;
	}
	Super::ItemPostFrame();
}

// =====================================================================================================================
// weapon_shotgun - CWeaponShotgun
// =====================================================================================================================

void ALambdaWeaponShotgun::InitializeFromScript(const FString& InClassName)
{
	Super::InitializeFromScript(InClassName);
	// sk_plr_num_shotgun_pellets / _double (hl2_gamerules.cpp), overridable from skill.cfg like every other one.
	NumPellets = FMath::RoundToInt(FSourceAmmoDef::Get().GetSkillValue(TEXT("sk_plr_num_shotgun_pellets"), 7.0f));
	NumPelletsDouble = FMath::RoundToInt(FSourceAmmoDef::Get().GetSkillValue(TEXT("sk_plr_num_shotgun_pellets_double"), 12.0f));
}

FVector ALambdaWeaponShotgun::GetBulletSpread() const
{
	// VECTOR_CONE_10DEGREES (basecombatweapon_shared.h)
	return FVector(0.08716f, 0.08716f, 0.08716f);
}

void ALambdaWeaponShotgun::FillClip()
{
	ALambdaCharacter* WeaponOwner = GetOwningCharacter();
	if (!WeaponOwner)
	{
		return;
	}
	if (WeaponOwner->GetAmmoCount(WeaponInfo.PrimaryAmmo) > 0 && Clip1 < WeaponInfo.ClipSize)
	{
		++Clip1;
		WeaponOwner->RemoveAmmo(WeaponInfo.PrimaryAmmo, 1);
	}
}

bool ALambdaWeaponShotgun::StartReload()
{
	ALambdaCharacter* WeaponOwner = GetOwningCharacter();
	if (!WeaponOwner)
	{
		return false;
	}
	if (WeaponOwner->GetAmmoCount(WeaponInfo.PrimaryAmmo) <= 0 || Clip1 >= WeaponInfo.ClipSize)
	{
		return false;
	}

	// An empty tube has to be pumped once the shells are in before anything will fire.
	if (Clip1 <= 0)
	{
		bNeedPump = true;
	}

	SendWeaponAnim(TEXT("ACT_SHOTGUN_RELOAD_START"));
	// Bodygroup 1 is the shell in the loader's hand, shown for as long as the reload runs.
	if (ALambdaCharacter* ShellOwner = GetOwningCharacter())
	{
		if (USourceStudioModelComponent* ViewModel = ShellOwner->GetViewModelMesh())
		{
			ViewModel->SetBodygroup(1, 0);
		}
	}

	NextPrimaryAttack = GetCurrentTime() + SequenceDuration();
	bInReload = true;
	return true;
}

bool ALambdaWeaponShotgun::Reload()
{
	// CWeaponShotgun::Reload: one shell, then back to ItemPostFrame to decide whether to go round again.
	ALambdaCharacter* WeaponOwner = GetOwningCharacter();
	if (!WeaponOwner)
	{
		return false;
	}
	if (WeaponOwner->GetAmmoCount(WeaponInfo.PrimaryAmmo) <= 0 || Clip1 >= WeaponInfo.ClipSize)
	{
		return false;
	}

	FillClip();
	WeaponSound(ESourceWeaponSound::Reload);
	SendWeaponAnim(TEXT("ACT_VM_RELOAD"));
	NextPrimaryAttack = GetCurrentTime() + SequenceDuration();
	return true;
}

void ALambdaWeaponShotgun::FinishReload()
{
	if (ALambdaCharacter* ShellOwner = GetOwningCharacter())
	{
		if (USourceStudioModelComponent* ViewModel = ShellOwner->GetViewModelMesh())
		{
			ViewModel->SetBodygroup(1, 1);	// put the loose shell away
		}
	}
	bInReload = false;
	SendWeaponAnim(TEXT("ACT_SHOTGUN_RELOAD_FINISH"));
	NextPrimaryAttack = GetCurrentTime() + SequenceDuration();
}

void ALambdaWeaponShotgun::Pump()
{
	bNeedPump = false;
	WeaponSound(ESourceWeaponSound::Special1);
	SendWeaponAnim(TEXT("ACT_SHOTGUN_PUMP"));
	NextPrimaryAttack = GetCurrentTime() + SequenceDuration();
}

void ALambdaWeaponShotgun::DryFire()
{
	WeaponSound(ESourceWeaponSound::Empty);
	SendWeaponAnim(TEXT("ACT_VM_DRYFIRE"));
	NextPrimaryAttack = GetCurrentTime() + SequenceDuration();
}

/** The pellet damage both attacks deal, from the ammo table's sk_plr_dmg_buckshot. */
static float ShotgunPelletDamage(const FSourceWeaponInfo& Info)
{
	if (const FSourceAmmoType* Ammo = FSourceAmmoDef::Get().Find(Info.PrimaryAmmo))
	{
		return Ammo->PlayerDamage;
	}
	return 0.0f;
}

void ALambdaWeaponShotgun::PrimaryAttack()
{
	ALambdaCharacter* WeaponOwner = GetOwningCharacter();
	if (!WeaponOwner)
	{
		return;
	}

	// The sound goes before the round comes out of the tube.
	WeaponSound(ESourceWeaponSound::Single);
	WeaponOwner->DoMuzzleFlash();
	SendWeaponAnim(TEXT("ACT_VM_PRIMARYATTACK"));

	// Nothing fires again until the firing animation has run out.
	NextPrimaryAttack = GetCurrentTime() + SequenceDuration();
	Clip1 -= 1;

	const float Damage = ShotgunPelletDamage(WeaponInfo);
	const FVector Spread = GetBulletSpread();
	for (int32 i = 0; i < NumPellets; ++i)
	{
		FireBullet(Damage, Spread);
	}

	WeaponOwner->ViewPunch(FRotator(FMath::FRandRange(-2.0f, -1.0f), FMath::FRandRange(-2.0f, 2.0f), 0.0f));

	// Pump, so long as there is anything left to chamber.
	if (Clip1)
	{
		bNeedPump = true;
	}
}

void ALambdaWeaponShotgun::SecondaryAttack()
{
	ALambdaCharacter* WeaponOwner = GetOwningCharacter();
	if (!WeaponOwner)
	{
		return;
	}

	WeaponSound(ESourceWeaponSound::Double);
	WeaponOwner->DoMuzzleFlash();
	SendWeaponAnim(TEXT("ACT_VM_SECONDARYATTACK"));

	NextPrimaryAttack = GetCurrentTime() + SequenceDuration();
	Clip1 -= 2;	// both barrels come out of the one tube

	const float Damage = ShotgunPelletDamage(WeaponInfo);
	const FVector Spread = GetBulletSpread();
	for (int32 i = 0; i < NumPelletsDouble; ++i)
	{
		FireBullet(Damage, Spread);
	}

	WeaponOwner->ViewPunch(FRotator(FMath::FRandRange(-5.0f, 5.0f), 0.0f, 0.0f));

	if (Clip1)
	{
		bNeedPump = true;
	}
}

void ALambdaWeaponShotgun::ItemPostFrame()
{
	// CWeaponShotgun::ItemPostFrame, which the shotgun owns outright rather than sharing with the base: the
	// reload is a loop it has to be able to break out of, and the pump has to happen between shots.
	ALambdaCharacter* WeaponOwner = GetOwningCharacter();
	if (!WeaponOwner || bHolstered)
	{
		return;
	}
	const float Now = GetCurrentTime();

	if (bInReload)
	{
		// One in the tube is enough: pulling the trigger cuts the reload short and fires.
		if (bAttackHeld && Clip1 >= 1)
		{
			bInReload = false;
			bNeedPump = false;
			bDelayedFire1 = true;
		}
		else if (bAttack2Held && Clip1 >= 2)
		{
			bInReload = false;
			bNeedPump = false;
			bDelayedFire2 = true;
		}
		else if (NextPrimaryAttack <= Now)
		{
			if (WeaponOwner->GetAmmoCount(WeaponInfo.PrimaryAmmo) <= 0)
			{
				FinishReload();
				return;
			}
			if (Clip1 < WeaponInfo.ClipSize)
			{
				Reload();	// round again, one more shell
				return;
			}
			FinishReload();
			return;
		}
	}
	else if (USourceStudioModelComponent* ViewModel = WeaponOwner->GetViewModelMesh())
	{
		ViewModel->SetBodygroup(1, 1);	// no reload running, so no loose shell
	}

	if (bNeedPump && NextPrimaryAttack <= Now)
	{
		Pump();
		return;
	}

	// Both barrels shares the primary's timer and the primary's shells.
	if ((bDelayedFire2 || bAttack2Held) && NextPrimaryAttack <= Now)
	{
		bDelayedFire2 = false;

		if (Clip1 <= 1 && UsesClipsForAmmo1())
		{
			// One shell left is a single shot, not a double.
			if (Clip1 == 1)
			{
				PrimaryAttack();
			}
			else if (!WeaponOwner->GetAmmoCount(WeaponInfo.PrimaryAmmo))
			{
				DryFire();
			}
			else
			{
				StartReload();
			}
		}
		else
		{
			if (bAttack2PressedThisFrame)
			{
				NextPrimaryAttack = Now;
			}
			SecondaryAttack();
		}
	}
	else if ((bDelayedFire1 || bAttackHeld) && NextPrimaryAttack <= Now)
	{
		bDelayedFire1 = false;
		if (Clip1 <= 0 && UsesClipsForAmmo1())
		{
			if (!WeaponOwner->GetAmmoCount(WeaponInfo.PrimaryAmmo))
			{
				DryFire();
			}
			else
			{
				StartReload();
			}
		}
		else
		{
			if (bAttackPressedThisFrame)
			{
				NextPrimaryAttack = Now;
			}
			PrimaryAttack();
		}
	}

	if (bReloadHeld && UsesClipsForAmmo1() && !bInReload)
	{
		StartReload();
	}
	else if (!bAttackHeld && !bAttack2Held)
	{
		// Nothing held: top the tube up on its own once the firing delay has passed, then idle.
		if (Clip1 <= 0 && NextPrimaryAttack < Now && StartReload())
		{
			return;
		}
		if (!bInReload)
		{
			WeaponIdle();
		}
	}

	bAttackPressedThisFrame = false;
	bAttack2PressedThisFrame = false;
}
