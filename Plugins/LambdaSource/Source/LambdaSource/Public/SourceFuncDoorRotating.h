#pragma once

#include "CoreMinimal.h"
#include "SourceBrushEntity.h"
#include "SourceFuncDoorRotating.generated.h"

/** doors.h spawnflags. */
namespace SourceDoorFlags
{
	enum Type : int32
	{
		SF_DOOR_ROTATE_YAW = 0,				// yaw by default
		SF_DOOR_START_OPEN_OBSOLETE = 1,
		SF_DOOR_ROTATE_BACKWARDS = 2,
		SF_DOOR_NONSOLID_TO_PLAYER = 4,
		SF_DOOR_PASSABLE = 8,
		SF_DOOR_ONEWAY = 16,
		SF_DOOR_NO_AUTO_RETURN = 32,
		SF_DOOR_ROTATE_ROLL = 64,
		SF_DOOR_ROTATE_PITCH = 128,
		SF_DOOR_PUSE = 256,					// door can be opened by player's use button
		SF_DOOR_NONPCS = 512,
		SF_DOOR_PTOUCH = 1024,				// player touch opens
		SF_DOOR_LOCKED = 2048,
		SF_DOOR_SILENT = 4096,
		SF_DOOR_USE_CLOSES = 8192,
		SF_DOOR_SILENT_TO_NPCS = 16384,
		SF_DOOR_IGNORE_USE = 32768,
		SF_DOOR_NEW_USE_RULES = 65536,
	};
}

/** CBaseToggle's TOGGLE_STATE. */
UENUM()
enum class ESourceToggleState : uint8
{
	AtTop,
	AtBottom,
	GoingUp,
	GoingDown
};

/** FuncDoorSpawnPos_t */
UENUM()
enum class ESourceDoorSpawnPos : uint8
{
	Closed = 0,
	Open = 1
};

/**
 * func_door_rotating - a port of CRotDoor / CBaseDoor (game/server/doors.cpp) on top of CBaseToggle's AngularMove
 * (game/server/subs.cpp). All angle math is done in Source's (pitch, yaw, roll) space and converted to an FRotator
 * only when the actor transform is written, so the ported logic matches the original sign-for-sign.
 */
UCLASS()
class LAMBDASOURCE_API ASourceFuncDoorRotating : public ASourceBrushEntity
{
	GENERATED_BODY()

public:
	ASourceFuncDoorRotating();

	virtual void InitializeFromEntity(const FSourceBSPFile& Map, int32 ModelIndex, const FSourceEntity& InEntity,
		ULambdaMaterialLibrary* MaterialLibrary, ASourceBSPWorldActor* InWorldActor) override;
	virtual void Tick(float DeltaSeconds) override;

	virtual bool IsUsable() const override;
	virtual void OnUsed(AActor* Activator) override;
	virtual bool AcceptInput(const FString& InputName, AActor* Activator, AActor* Caller, const FString& Parameter) override;

	// ---- CBaseDoor inputs ----
	void InputOpen();
	void InputClose();
	void InputToggle();
	void Lock() { bLocked = true; }
	void Unlock() { bLocked = false; }
	/** CRotDoor::SetToggleState */
	void SetToggleState(ESourceToggleState State);

	/** CBaseDoor::DoorActivate - decides whether to open or close. */
	int32 DoorActivate();

	/** CBaseDoor::DoorTouch - a player touching the door opens it when SF_DOOR_PTOUCH is set. */
	void DoorTouch(AActor* Other);

	UFUNCTION(BlueprintPure, Category = "Lambda")
	ESourceToggleState GetToggleState() const { return ToggleState; }

protected:
	// ---- Sounds (CBaseDoor) ----
	/** noise1 while moving (looping), noise2 on arrival. Suppressed by SF_DOOR_SILENT. */
	void StartMovingSound();
	void StopMovingSound();
	void PlayArrivedSound();
	/** PlayLockSounds() - locked_sound when refused, unlocked_sound when it opens. */
	void PlayLockSounds(bool bLockedSound);

	// ---- Blocked (CBaseDoor::StartBlocked / Blocked / EndBlocked) ----
	void CheckBlocked();
	void StartBlocked(AActor* Other);
	void Blocked(AActor* Other);
	void EndBlocked();

	// ---- CBaseDoor state machine ----
	void DoorGoUp();
	void DoorGoDown();
	void DoorHitTop();
	void DoorHitBottom();

	// ---- CBaseToggle ----
	/** Sets m_vecMoveAng from the rotate-axis spawnflags. */
	void AxisDir();
	/** Starts an angular move toward DestAngle at flSpeed degrees/sec. */
	void AngularMove(const FVector3f& DestAngle, float InSpeed);
	void AngularMoveDone();
	/** Called when the current move (or wait) elapses. */
	void MoveDone();
	void SetMoveDoneTime(float Delay);

	/** Length of a QAngle, as Source computes it. */
	static float AngleLength(const FVector3f& A) { return FMath::Sqrt(A.X * A.X + A.Y * A.Y + A.Z * A.Z); }

	// ---- CBaseToggle members ----
	ESourceToggleState ToggleState = ESourceToggleState::AtBottom;
	FVector3f MoveAng = FVector3f::ZeroVector;		// m_vecMoveAng
	FVector3f Angle1 = FVector3f::ZeroVector;		// m_vecAngle1 (closed)
	FVector3f Angle2 = FVector3f::ZeroVector;		// m_vecAngle2 (open)
	FVector3f FinalAngle = FVector3f::ZeroVector;	// m_vecFinalAngle
	FVector3f AngularVelocity = FVector3f::ZeroVector;
	float MoveDistance = 90.0f;						// m_flMoveDistance ("distance")
	float Speed = 100.0f;							// m_flSpeed ("speed")
	float Wait = 4.0f;								// m_flWait ("wait")
	bool bLocked = false;							// m_bLocked
	ESourceDoorSpawnPos SpawnPosition = ESourceDoorSpawnPos::Closed;

	/** Remaining time until MoveDone fires; < 0 means no pending move. */
	float MoveDoneTime = -1.0f;
	/** What MoveDone should do when it fires. */
	enum class EPendingMove : uint8 { None, HitTop, HitBottom, GoDown };
	EPendingMove PendingMove = EPendingMove::None;

	TWeakObjectPtr<AActor> Activator;				// m_hActivator

	// Sounds named directly by the map (Source resolves these through soundscripts too, but map-authored wav
	// paths like "doors/door_locked2.wav" are used as-is).
	FString LockedSound;			// locked_sound
	FString UnlockedSound;			// unlocked_sound
	float LockSoundWaitTime = 0.0f;	// locksound_t::flwaitSound - next time a lock/unlock sound may play

	float BlockDamage = 0.0f;		// m_flBlockDamage ("dmg")
	bool bForceClosed = false;		// m_bForceClosed ("forceclosed")
	TWeakObjectPtr<AActor> Blocker;	// who is currently blocking us

	/** Stands in for SetTouch(&CBaseDoor::DoorTouch) / SetTouch(NULL): touch is disabled while the door moves. */
	bool bTouchEnabled = true;

	// Sound names from the entity keyvalues.
	FString NoiseMoving;			// m_NoiseMoving   ("noise1")
	FString NoiseArrived;			// m_NoiseArrived  ("noise2")
	FString NoiseMovingClosed;		// m_NoiseMovingClosed ("startclosesound")
	FString NoiseArrivedClosed;		// m_NoiseArrivedClosed ("closesound")

	UPROPERTY(Transient)
	TObjectPtr<class UAudioComponent> MovingAudio;
};
