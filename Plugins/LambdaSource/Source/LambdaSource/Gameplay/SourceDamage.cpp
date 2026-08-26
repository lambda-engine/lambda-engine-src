#include "Gameplay/SourceDamage.h"

int32 SourceDamage::TypeFromName(const FString& Name)
{
	using namespace SourceDamageType;
	static const TMap<FString, int32> Types = {
		{ TEXT("generic"), DMG_GENERIC },	{ TEXT("crush"), DMG_CRUSH },
		{ TEXT("bullet"), DMG_BULLET },		{ TEXT("slash"), DMG_SLASH },
		{ TEXT("burn"), DMG_BURN },			{ TEXT("fall"), DMG_FALL },
		{ TEXT("blast"), DMG_BLAST },		{ TEXT("club"), DMG_CLUB },
		{ TEXT("shock"), DMG_SHOCK },		{ TEXT("sonic"), DMG_SONIC },
		{ TEXT("buckshot"), DMG_BUCKSHOT },	{ TEXT("drown"), DMG_DROWN },
		{ TEXT("paralyze"), DMG_PARALYZE },	{ TEXT("nervegas"), DMG_NERVEGAS },
		{ TEXT("poison"), DMG_POISON },		{ TEXT("radiation"), DMG_RADIATION },
		{ TEXT("acid"), DMG_ACID },			{ TEXT("slowburn"), DMG_SLOWBURN },
		{ TEXT("plasma"), DMG_PLASMA },		{ TEXT("sniper"), DMG_SNIPER },
	};
	const int32* Found = Types.Find(Name.ToLower());
	return Found ? *Found : DMG_GENERIC;
}
