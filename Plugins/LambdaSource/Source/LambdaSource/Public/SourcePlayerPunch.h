#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "SourcePlayerPunch.generated.h"

UINTERFACE(MinimalAPI)
class USourcePlayerPunch : public UInterface
{
	GENERATED_BODY()
};

/**
 * CBasePlayer::ViewPunch / CBaseCombatCharacter::VelocityPunch: what a melee hit does to the player besides
 * damage. Implemented by the player character in the game module.
 */
class LAMBDASOURCE_API ISourcePlayerPunch
{
	GENERATED_BODY()

public:
	/** ViewPunch: kicks the view by these angles (pitch, yaw, roll in degrees); it springs back on its own. */
	virtual void ViewPunch(const FRotator& AngleOffset) = 0;
	/** VelocityPunch: an instantaneous velocity impulse, cm/s. */
	virtual void VelocityPunch(const FVector& VelocityImpulse) = 0;
};
