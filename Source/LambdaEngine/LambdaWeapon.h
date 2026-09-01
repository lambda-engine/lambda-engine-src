#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Weapons/SourceWeaponScript.h"
#include "LambdaWeapon.generated.h"

class ALambdaCharacter;

/** CBaseCombatWeapon's WeaponSound_t, keyed by the SoundData names in the weapon script. */
UENUM()
enum class ESourceWeaponSound : uint8
{
	Empty,
	Single,
	Double,			// WPN_DOUBLE - the shotgun's both-barrels blast
	Burst,
	Special1,
	Special2,
	Reload,
	Melee_Miss,
	Melee_Hit,
	Melee_HitWorld
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
	/** CBaseCombatWeapon::SecondaryAttack - IN_ATTACK2. Most weapons have nothing on it. */
	virtual void SecondaryAttack() {}
	virtual bool Reload();
	/** CBaseCombatWeapon::HandleFireOnEmpty. */
	virtual void HandleFireOnEmpty();

	/**
	 * CBaseCombatWeapon::Holster - the weapon is put away: the view model goes out of sight and the weapon stops
	 * taking input. This is what happens when the player picks a physics prop up with both hands.
	 */
	virtual void Holster();
	/** CBaseCombatWeapon::Deploy - the weapon comes back out. */
	virtual void Deploy();
	bool IsHolstered() const { return bHolstered; }

	void SetOwningCharacter(ALambdaCharacter* InOwner) { OwningCharacter = InOwner; }
	ALambdaCharacter* GetOwningCharacter() const { return OwningCharacter.Get(); }

	/** Plays one of the weapon script's SoundData entries through the soundscript system. */
	void WeaponSound(ESourceWeaponSound Sound);

	/** CBaseCombatWeapon::SendWeaponAnim - plays the view-model sequence for an activity and re-arms the idle timer. */
	bool SendWeaponAnim(const FString& ActivityName);
	/** CBaseAnimating::SequenceDuration for the view-model sequence now playing. */
	float SequenceDuration() const;
	/** CBaseCombatWeapon::WeaponIdle - returns to ACT_VM_IDLE once the current animation has run out. */
	virtual void WeaponIdle();
	bool HasWeaponIdleTimeElapsed() const { return GetCurrentTime() > TimeWeaponIdle; }
	void SetWeaponIdleTime(float Time) { TimeWeaponIdle = Time; }

	UFUNCTION(BlueprintPure, Category = "Lambda") int32 GetClip1() const { return Clip1; }
	UFUNCTION(BlueprintPure, Category = "Lambda") int32 GetClipSize() const { return WeaponInfo.ClipSize; }
	UFUNCTION(BlueprintPure, Category = "Lambda") FString GetWeaponClassName() const { return WeaponInfo.ClassName; }
	FString GetPrimaryAmmoType() const { return WeaponInfo.PrimaryAmmo; }
	const FSourceWeaponInfo& GetWeaponInfo() const { return WeaponInfo; }
	bool UsesClipsForAmmo1() const { return WeaponInfo.UsesClipsForAmmo1(); }

	/** Set from the player's input each frame (CBasePlayer::m_nButtons IN_ATTACK / IN_ATTACK2 / IN_RELOAD). */
	bool bAttackHeld = false;
	bool bAttackPressedThisFrame = false;
	bool bAttack2Held = false;
	bool bAttack2PressedThisFrame = false;
	bool bReloadHeld = false;

protected:
	/** Fires one bullet: a trace from the eye along the view direction, spread applied. */
	void FireBullet(float Damage, const FVector& Spread);
	/** CBaseCombatWeapon::GetFireRate - seconds between shots. */
	virtual float GetFireRate() const { return 0.5f; }
	/** The cone this weapon fires within, in the Source VECTOR_CONE_* form (sin of half-angle per axis). */
	virtual FVector GetBulletSpread() const { return FVector::ZeroVector; }
	/** Activity played on a primary attack (CBaseCombatWeapon::GetPrimaryAttackActivity). */
	virtual FString GetPrimaryAttackActivity() const { return TEXT("ACT_VM_PRIMARYATTACK"); }

	float GetCurrentTime() const;

	float TimeWeaponIdle = 0.0f;		// m_flTimeWeaponIdle

	FSourceWeaponInfo WeaponInfo;

	int32 Clip1 = 0;					// m_iClip1
	float NextPrimaryAttack = 0.0f;		// m_flNextPrimaryAttack
	float NextSecondaryAttack = 0.0f;	// m_flNextSecondaryAttack
	/** How long the trigger has been held, which is what the machine guns build their recoil from. */
	float FireDuration = 0.0f;			// m_fFireDuration
	float NextEmptySoundTime = 0.0f;	// m_flNextEmptySoundTime
	bool bInReload = false;				// m_bInReload
	bool bHolstered = false;
	float ReloadFinishTime = 0.0f;

	UPROPERTY(Transient)
	TWeakObjectPtr<ALambdaCharacter> OwningCharacter;
};

/**
 * weapon_pistol - a port of CWeaponPistol (game/server/hl2/weapon_pistol.cpp). The refire and accuracy constants are
 * that file's; the clip size, ammo type and sounds come from scripts/weapon_pistol.txt; the damage comes from
 * sk_plr_dmg_pistol in cfg/skill.cfg.
 */
/**
 * weapon_crowbar - a port of CWeaponCrowbar / CBaseHLBludgeonWeapon (game/server/hl2/weapon_crowbar.cpp,
 * basebludgeonweapon.cpp). One swing per CROWBAR_REFIRE: a ray out to CROWBAR_RANGE, then the bludgeon hull when
 * the ray misses, dealing sk_plr_dmg_crowbar of DMG_CLUB with a melee shove behind it. Hits play the flesh or
 * world impact sound and stamp the surface's decal; the view punches the way HL2's crowbar does.
 */
UCLASS()
class LAMBDAENGINE_API ALambdaWeaponCrowbar : public ALambdaWeapon
{
	GENERATED_BODY()

public:
	virtual void InitializeFromScript(const FString& InClassName) override;
	virtual void ItemPostFrame() override;

protected:
	/** CBaseHLBludgeonWeapon::Swing. */
	void Swing();
	virtual float GetFireRate() const override { return 0.4f; }	// CROWBAR_REFIRE

	float DamagePerSwing = 10.0f;	// sk_plr_dmg_crowbar
};

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

/**
 * weapon_smg1 - CWeaponSMG1 (game/server/hl2/weapon_smg1.cpp) standing on CHLMachineGun
 * (game/server/hl2/basehlcombatweapon.cpp).
 *
 * Held down it fires at 13.3hz within a five degree cone, and the view climbs the longer it is held - the recoil
 * is a function of how long the trigger has been down, not of how many rounds have gone. The view model runs up a
 * ladder of recoil animations for the first few shots so the gun does not simply repeat one frame.
 *
 * Its secondary attack in Half-Life 2 is the grenade launcher, which wants a grenade to launch; that is not here.
 */
UCLASS()
class LAMBDAENGINE_API ALambdaWeaponSMG1 : public ALambdaWeapon
{
	GENERATED_BODY()

public:
	virtual void ItemPostFrame() override;
	virtual void PrimaryAttack() override;

protected:
	virtual float GetFireRate() const override { return 0.075f; }	// 13.3hz
	virtual FVector GetBulletSpread() const override;				// VECTOR_CONE_5DEGREES
	virtual FString GetPrimaryAttackActivity() const override;
	/** CWeaponSMG1::AddViewKick -> CHLMachineGun::DoMachineGunKick. */
	void AddViewKick();

	// weapon_smg1.cpp's AddViewKick
	static constexpr float EASY_DAMPEN = 0.5f;
	static constexpr float MAX_VERTICAL_KICK = 1.0f;	// degrees
	static constexpr float SLIDE_LIMIT = 2.0f;			// seconds

	int32 NumShotsFired = 0;			// m_nShotsFired
};

/**
 * weapon_frag - the hand grenade (CWeaponFrag).
 *
 * The only weapon that leaves the player's hands: the attack is a throw, and what it throws is the same
 * ASourceGrenade a Combine soldier lobs. Source pulls the pin on the press and releases on the let-go,
 * so a held grenade cooks; ours throws on the press, which is the half of it that matters for now.
 */
UCLASS()
class LAMBDAENGINE_API ALambdaWeaponFrag : public ALambdaWeapon
{
	GENERATED_BODY()

public:
	virtual void PrimaryAttack() override;
	virtual void SecondaryAttack() override;

protected:
	/** CWeaponFrag::ThrowGrenade / LobGrenade - the same grenade, thrown hard or lobbed underarm. */
	void ThrowGrenade(bool bLob);
};

/**
 * weapon_shotgun - CWeaponShotgun (game/server/hl2/weapon_shotgun.cpp).
 *
 * Two things make it what it is. It is pumped between shots rather than after them: firing sets a flag, and the
 * pump happens on the next frame the weapon is free, so the shot goes off the moment the trigger does and the
 * work is paid for afterwards. And it reloads a shell at a time, in a loop the player can break out of by
 * pulling the trigger - one round in the tube is enough to fire.
 *
 * Both barrels on secondary: twelve pellets instead of seven, two shells instead of one, and a harder kick.
 */
UCLASS()
class LAMBDAENGINE_API ALambdaWeaponShotgun : public ALambdaWeapon
{
	GENERATED_BODY()

public:
	virtual void InitializeFromScript(const FString& InClassName) override;
	virtual void ItemPostFrame() override;
	virtual void PrimaryAttack() override;
	virtual void SecondaryAttack() override;
	/** CWeaponShotgun::Reload - puts one shell in. StartReload begins the loop this runs inside. */
	virtual bool Reload() override;

protected:
	virtual FVector GetBulletSpread() const override;	// VECTOR_CONE_10DEGREES

	/** CWeaponShotgun::StartReload - opens the reload, and asks for a pump if the tube was empty. */
	bool StartReload();
	void FinishReload();
	void FillClip();
	void Pump();
	void DryFire();

	bool bNeedPump = false;		// m_bNeedPump
	bool bDelayedFire1 = false;	// m_bDelayedFire1 - fire interrupted a reload
	bool bDelayedFire2 = false;	// m_bDelayedFire2

	/** sk_plr_num_shotgun_pellets / _double, from hl2_gamerules.cpp. */
	int32 NumPellets = 7;
	int32 NumPelletsDouble = 12;
};
