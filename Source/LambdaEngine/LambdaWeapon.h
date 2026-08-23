#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SourceWeaponScript.h"
#include "LambdaWeapon.generated.h"

class ALambdaCharacter;

/** CBaseCombatWeapon's WeaponSound_t, keyed by the SoundData names in the weapon script. */
UENUM()
enum class ESourceWeaponSound : uint8
{
	Empty,
	Single,
	Special1,
	Special2,
	Reload,
	Melee_Miss,
	Melee_Hit
};

/**
 * A port of the parts of CBaseCombatWeapon (game/shared/basecombatweapon_shared.cpp) a first-person shooter needs:
 * clip tracking, the attack/reload timing in ItemPostFrame, and firing bullets.
 *
 * The weapon's data - clip size, ammo type and sounds - comes from scripts/weapon_<name>.txt, exactly as in Source.
 */
UCLASS()
class LAMBDAENGINE_API ALambdaWeapon : public AActor
{
	GENERATED_BODY()

public:
	ALambdaWeapon();

	/** Loads the weapon script and fills the clip, as Spawn() does. */
	virtual void InitializeFromScript(const FString& InClassName);

	virtual void Tick(float DeltaSeconds) override;

	/** CBaseCombatWeapon::ItemPostFrame - runs every frame while the weapon is active. */
	virtual void ItemPostFrame();

	virtual void PrimaryAttack();
	virtual bool Reload();
	/** CBaseCombatWeapon::HandleFireOnEmpty. */
	virtual void HandleFireOnEmpty();

	void SetOwningCharacter(ALambdaCharacter* InOwner) { OwningCharacter = InOwner; }
	ALambdaCharacter* GetOwningCharacter() const { return OwningCharacter.Get(); }

	/** Plays one of the weapon script's SoundData entries through the soundscript system. */
	void WeaponSound(ESourceWeaponSound Sound);

	UFUNCTION(BlueprintPure, Category = "Lambda") int32 GetClip1() const { return Clip1; }
	UFUNCTION(BlueprintPure, Category = "Lambda") int32 GetClipSize() const { return WeaponInfo.ClipSize; }
	UFUNCTION(BlueprintPure, Category = "Lambda") FString GetWeaponClassName() const { return WeaponInfo.ClassName; }
	FString GetPrimaryAmmoType() const { return WeaponInfo.PrimaryAmmo; }
	const FSourceWeaponInfo& GetWeaponInfo() const { return WeaponInfo; }
	bool UsesClipsForAmmo1() const { return WeaponInfo.UsesClipsForAmmo1(); }

	/** Set from the player's input each frame (CBasePlayer::m_nButtons IN_ATTACK / IN_RELOAD). */
	bool bAttackHeld = false;
	bool bAttackPressedThisFrame = false;
	bool bReloadHeld = false;

protected:
	/** Fires one bullet: a trace from the eye along the view direction, spread applied. */
	void FireBullet(float Damage, const FVector& Spread);
	/** CBaseCombatWeapon::GetFireRate - seconds between shots. */
	virtual float GetFireRate() const { return 0.5f; }
	/** The cone this weapon fires within, in the Source VECTOR_CONE_* form (sin of half-angle per axis). */
	virtual FVector GetBulletSpread() const { return FVector::ZeroVector; }
	/** Seconds the reload takes; Source drives this from the view-model animation, which we do not have. */
	virtual float GetReloadTime() const { return 1.5f; }

	float GetCurrentTime() const;

	FSourceWeaponInfo WeaponInfo;

	int32 Clip1 = 0;					// m_iClip1
	float NextPrimaryAttack = 0.0f;		// m_flNextPrimaryAttack
	float NextEmptySoundTime = 0.0f;	// m_flNextEmptySoundTime
	bool bInReload = false;				// m_bInReload
	float ReloadFinishTime = 0.0f;

	UPROPERTY(Transient)
	TWeakObjectPtr<ALambdaCharacter> OwningCharacter;
};

/**
 * weapon_pistol - a port of CWeaponPistol (game/server/hl2/weapon_pistol.cpp). The refire and accuracy constants are
 * that file's; the clip size, ammo type and sounds come from scripts/weapon_pistol.txt; the damage comes from
 * sk_plr_dmg_pistol in cfg/skill.cfg.
 */
UCLASS()
class LAMBDAENGINE_API ALambdaWeaponPistol : public ALambdaWeapon
{
	GENERATED_BODY()

public:
	ALambdaWeaponPistol();

	virtual void ItemPostFrame() override;
	virtual void PrimaryAttack() override;

protected:
	virtual FVector GetBulletSpread() const override;
	virtual float GetFireRate() const override { return 0.5f; }	// CBaseCombatWeapon's default; the pistol gates on m_flSoonestPrimaryAttack
	void DryFire();

	// weapon_pistol.cpp
	static constexpr float PISTOL_FASTEST_REFIRE_TIME = 0.1f;
	static constexpr float PISTOL_FASTEST_DRY_REFIRE_TIME = 0.2f;
	static constexpr float PISTOL_ACCURACY_SHOT_PENALTY_TIME = 0.2f;
	static constexpr float PISTOL_ACCURACY_MAXIMUM_PENALTY_TIME = 1.5f;

	float SoonestPrimaryAttack = 0.0f;	// m_flSoonestPrimaryAttack
	float LastAttackTime = 0.0f;		// m_flLastAttackTime
	float AccuracyPenalty = 0.0f;		// m_flAccuracyPenalty
	int32 NumShotsFired = 0;			// m_nNumShotsFired
};
