#include "LambdaWeapon.h"
#include "LambdaCharacter.h"
#include "SourceStudioModelComponent.h"
#include "LambdaEngine.h"
#include "LambdaSoundLibrary.h"
#include "LambdaSourceSettings.h"
#include "SourceAmmoDef.h"
#include "SourceCoordinates.h"
#include "SourceImpactEffects.h"
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
	case ESourceWeaponSound::Reload:	Key = TEXT("reload"); break;
	case ESourceWeaponSound::Special1:	Key = TEXT("special1"); break;
	case ESourceWeaponSound::Special2:	Key = TEXT("special2"); break;
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
	if (!WeaponOwner || !WeaponOwner->SendViewModelAnim(ActivityName))
	{
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

void ALambdaWeapon::ItemPostFrame()
{
	// CBaseCombatWeapon::ItemPostFrame
	ALambdaCharacter* WeaponOwner = OwningCharacter.Get();
	if (!WeaponOwner)
	{
		return;
	}

	const float Now = GetCurrentTime();

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

	// Reload pressed
	if (bReloadHeld && UsesClipsForAmmo1() && !bInReload)
	{
		Reload();
	}

	// No buttons down: let the weapon drift back to its idle animation.
	if (!bAttackHeld && !bReloadHeld && !bInReload)
	{
		WeaponIdle();
	}

	bAttackPressedThisFrame = false;
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
	if (World->LineTraceSingleByChannel(Hit, EyeLocation, End, ECC_Visibility, Params))
	{
		UE_LOG(LogLambda, Verbose, TEXT("bullet hit %s at %s (damage %g)"),
			*GetNameSafe(Hit.GetActor()), *Hit.ImpactPoint.ToString(), Damage);

		// UTIL_ImpactTrace: the decal and impact sound come from the surface that was struck.
		SourceImpact::PlayImpact(Hit, WeaponOwner->GetWorldMaterialLibrary(), this);

		// Nothing in the map takes damage yet (no NPCs or breakables), so this is where TakeDamage would go.
		if (AActor* HitActor = Hit.GetActor())
		{
			HitActor->TakeDamage(Damage, FDamageEvent(), WeaponOwner->GetController(), WeaponOwner);
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
