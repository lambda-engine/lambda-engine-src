#pragma once

#include "CoreMinimal.h"

/**
 * One ammo type, as registered by CHalfLife2::Init()'s def.AddAmmoType() table (game/shared/hl2/hl2_gamerules.cpp).
 * The damage and carry limit are named skill cvars whose values come from cfg/skill.cfg, so they stay data-driven.
 */
struct LAMBDASOURCE_API FSourceAmmoType
{
	FString Name;				// "Pistol"
	FString PlayerDamageCvar;	// "sk_plr_dmg_pistol"
	FString NpcDamageCvar;		// "sk_npc_dmg_pistol"
	FString MaxCarryCvar;		// "sk_max_pistol"

	float PlayerDamage = 0.0f;
	float DamageForce = 0.0f;	// physicsForceImpulse, kg*in/s (BULLET_IMPULSE)
	float MaxCarry = 0.0f;
};

/**
 * The ammo table plus cfg/skill.cfg. skill.cfg is a list of "cvar value" lines that Source execs at startup to set
 * the difficulty-dependent damage and carry limits.
 */
class LAMBDASOURCE_API FSourceAmmoDef
{
public:
	static FSourceAmmoDef& Get();

	void Initialize();
	void Reset() { AmmoTypes.Reset(); SkillValues.Reset(); bInitialized = false; }

	const FSourceAmmoType* Find(const FString& AmmoName);

	/** Value of a skill cvar from skill.cfg, or Default. */
	float GetSkillValue(const FString& CvarName, float Default = 0.0f);

private:
	FSourceAmmoDef() = default;
	void LoadSkillConfig();
	void AddAmmoType(const FString& Name, const FString& PlayerDamageCvar, const FString& NpcDamageCvar, const FString& MaxCarryCvar,
		float DamageForce = 0.0f);

	TMap<FString, FSourceAmmoType> AmmoTypes;
	TMap<FString, float> SkillValues;
	bool bInitialized = false;
};
