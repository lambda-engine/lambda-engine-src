#pragma once

#include "CoreMinimal.h"
#include "SourceBrushEntity.h"
#include "SourceFuncDoorRotating.h"	// ESourceToggleState
#include "SourceFuncButton.generated.h"

/** buttons.cpp spawnflags. */
namespace SourceButtonFlags
{
	enum Type : int32
	{
		SF_BUTTON_DONTMOVE = 1,
		SF_BUTTON_TOGGLE = 32,				// button stays pushed until reactivated
		SF_BUTTON_TOUCH_ACTIVATES = 256,
		SF_BUTTON_DAMAGE_ACTIVATES = 512,
		SF_BUTTON_USE_ACTIVATES = 1024,
		SF_BUTTON_LOCKED = 2048,
		SF_BUTTON_SPARK_IF_OFF = 4096,
		SF_BUTTON_JIGGLE_ON_USE_LOCKED = 8192,
		SF_BUTTON_NOTSOLID = 16384,
	};
}

/**
 * func_button - a port of CBaseButton (game/server/buttons.cpp) moving on CBaseToggle::LinearMove
 * (game/server/subs.cpp). Positions are kept in Source units and converted only when the transform is written.
 */
UCLASS()
class LAMBDASOURCE_API ASourceFuncButton : public ASourceBrushEntity
{
	GENERATED_BODY()

public:
	ASourceFuncButton();

	virtual void InitializeFromEntity(const FSourceBSPFile& Map, int32 ModelIndex, const FSourceEntity& InEntity,
		ULambdaMaterialLibrary* MaterialLibrary, ASourceBSPWorldActor* InWorldActor) override;
	virtual void Tick(float DeltaSeconds) override;

	virtual bool IsUsable() const override;
	virtual void OnUsed(AActor* Activator) override;
	virtual bool AcceptInput(const FString& InputName, AActor* Activator, AActor* Caller, const FString& Parameter) override;

	UFUNCTION(BlueprintPure, Category = "Lambda")
	ESourceToggleState GetToggleState() const { return ToggleState; }

protected:
	/** CBaseButton::BUTTON_CODE */
	enum class EButtonCode : uint8 { Nothing, Activate, Return, Press };

	// ---- CBaseButton ----
	void ButtonUse(AActor* Activator);
	void ButtonTouch(AActor* Other);
	EButtonCode ButtonResponseToTouch() const;
	void Press(AActor* Activator, EButtonCode Code);
	void ButtonActivate();
	void TriggerAndWait();
	void ButtonReturn();
	void ButtonBackHome();
	bool OnUseLocked(AActor* Activator);
	void Lock() { bLocked = true; }
	void Unlock() { bLocked = false; }

	// ---- CBaseToggle::LinearMove ----
	void LinearMove(const FVector3f& DestPosition, float InSpeed);
	void LinearMoveDone();
	void MoveDone();
	void SetMoveDoneTime(float Delay) { MoveDoneTime = Delay; }

	void PlayButtonSound();
	void PlayLockSounds(bool bLockedSound);

	ESourceToggleState ToggleState = ESourceToggleState::AtBottom;
	FVector3f MoveDir = FVector3f::ZeroVector;		// m_vecMoveDir (a direction, from the "movedir" angles)
	FVector3f Position1 = FVector3f::ZeroVector;	// m_vecPosition1 (out)
	FVector3f Position2 = FVector3f::ZeroVector;	// m_vecPosition2 (pressed in)
	FVector3f FinalDest = FVector3f::ZeroVector;	// m_vecFinalDest
	FVector3f Velocity = FVector3f::ZeroVector;
	float Speed = 40.0f;							// m_flSpeed
	float Wait = 1.0f;								// m_flWait
	float Lip = 4.0f;								// m_flLip
	bool bStayPushed = false;						// m_fStayPushed
	bool bLocked = false;							// m_bLocked
	int32 Sounds = 0;								// m_sounds
	float UseLockedTime = 0.0f;						// m_flUseLockedTime

	FString NoiseButton;			// m_sNoise, resolved from "sounds"
	FString LockedSound;			// locked_sound
	FString UnlockedSound;			// unlocked_sound

	float MoveDoneTime = -1.0f;
	enum class EPendingMove : uint8 { None, TriggerAndWait, BackHome, Return };
	EPendingMove PendingMove = EPendingMove::None;

	bool bTouchEnabled = false;
	TWeakObjectPtr<AActor> Activator;				// m_hActivator
};
