#include "Weapons/SourceAmmoDef.h"
#include "Gameplay/SourceDamage.h"
#include "FileSystem/LambdaFileSystem.h"
#include "Core/LambdaSourceModule.h"

FSourceAmmoDef& FSourceAmmoDef::Get()
{
	static FSourceAmmoDef Instance;
	return Instance;
}

void FSourceAmmoDef::AddAmmoType(const FString& Name, const FString& PlayerDamageCvar, const FString& NpcDamageCvar, const FString& MaxCarryCvar,
	float DamageForce)
{
	FSourceAmmoType Type;
	Type.Name = Name;
	Type.DamageForce = DamageForce;
	Type.PlayerDamageCvar = PlayerDamageCvar;
	Type.NpcDamageCvar = NpcDamageCvar;
	Type.MaxCarryCvar = MaxCarryCvar;
	Type.PlayerDamage = GetSkillValue(PlayerDamageCvar, 0.0f);
	Type.MaxCarry = GetSkillValue(MaxCarryCvar, 0.0f);
	AmmoTypes.Add(Name.ToLower(), MoveTemp(Type));
}

void FSourceAmmoDef::Initialize()
{
	if (bInitialized)
	{
		return;
	}
	bInitialized = true;

	LoadSkillConfig();

	// The subset of CHalfLife2::Init()'s AddAmmoType() table that the shipped weapon scripts reference.
	// Forces are BULLET_IMPULSE(grains, ft/s) from the same table; the 357 and crossbow hit like trucks on purpose.
	AddAmmoType(TEXT("Pistol"),			TEXT("sk_plr_dmg_pistol"),		TEXT("sk_npc_dmg_pistol"),		TEXT("sk_max_pistol"),		SourceDamage::BulletImpulse(200, 1225));
	AddAmmoType(TEXT("SMG1"),			TEXT("sk_plr_dmg_smg1"),		TEXT("sk_npc_dmg_smg1"),		TEXT("sk_max_smg1"),		SourceDamage::BulletImpulse(200, 1225));
	AddAmmoType(TEXT("357"),			TEXT("sk_plr_dmg_357"),			TEXT("sk_npc_dmg_357"),			TEXT("sk_max_357"),			SourceDamage::BulletImpulse(800, 5000));
	AddAmmoType(TEXT("AR2"),			TEXT("sk_plr_dmg_ar2"),			TEXT("sk_npc_dmg_ar2"),			TEXT("sk_max_ar2"),			SourceDamage::BulletImpulse(200, 1225));
	AddAmmoType(TEXT("Buckshot"),		TEXT("sk_plr_dmg_buckshot"),	TEXT("sk_npc_dmg_buckshot"),	TEXT("sk_max_buckshot"),	SourceDamage::BulletImpulse(400, 1200));
	AddAmmoType(TEXT("XBowBolt"),		TEXT("sk_plr_dmg_crossbow"),	TEXT("sk_npc_dmg_crossbow"),	TEXT("sk_max_crossbow"),	SourceDamage::BulletImpulse(800, 8000));
	AddAmmoType(TEXT("Grenade"),		TEXT("sk_plr_dmg_grenade"),		TEXT("sk_npc_dmg_grenade"),		TEXT("sk_max_grenade"));
	AddAmmoType(TEXT("RPG_Round"),		TEXT("sk_plr_dmg_rpg_round"),	TEXT("sk_npc_dmg_rpg_round"),	TEXT("sk_max_rpg_round"));

	for (const TPair<FString, FSourceAmmoType>& Pair : AmmoTypes)
	{
		UE_LOG(LogLambdaSource, Verbose, TEXT("  ammo %s: damage=%g maxcarry=%g"), *Pair.Value.Name, Pair.Value.PlayerDamage, Pair.Value.MaxCarry);
	}
	UE_LOG(LogLambdaSource, Log, TEXT("Ammo types: %d (skill.cfg values: %d)"), AmmoTypes.Num(), SkillValues.Num());
}

void FSourceAmmoDef::LoadSkillConfig()
{
	// skill.cfg is a plain list of `cvar "value"` lines that Source execs to apply the difficulty settings.
	FString Text;
	if (!FLambdaFileSystem::Get().ReadFileToString(TEXT("cfg/skill.cfg"), Text))
	{
		UE_LOG(LogLambdaSource, Warning, TEXT("No cfg/skill.cfg found - weapon damage and ammo limits will be zero"));
		return;
	}

	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	for (FString Line : Lines)
	{
		// Strip comments and surrounding whitespace.
		int32 Comment = INDEX_NONE;
		if (Line.FindChar(TEXT('/'), Comment) && Line.IsValidIndex(Comment + 1) && Line[Comment + 1] == TEXT('/'))
		{
			Line.LeftInline(Comment);
		}
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty())
		{
			continue;
		}

		TArray<FString> Tokens;
		Line.ParseIntoArrayWS(Tokens);
		if (Tokens.Num() < 2)
		{
			continue;
		}
		FString Value = Tokens[1];
		Value.TrimQuotesInline();
		SkillValues.Add(Tokens[0].ToLower(), FCString::Atof(*Value));
	}
}

float FSourceAmmoDef::GetSkillValue(const FString& CvarName, float Default)
{
	if (const float* Found = SkillValues.Find(CvarName.ToLower()))
	{
		return *Found;
	}
	return Default;
}

const FSourceAmmoType* FSourceAmmoDef::Find(const FString& AmmoName)
{
	Initialize();
	return AmmoTypes.Find(AmmoName.ToLower());
}
