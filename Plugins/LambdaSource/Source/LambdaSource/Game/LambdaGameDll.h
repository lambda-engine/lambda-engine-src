#pragma once

#include "CoreMinimal.h"

// The contract. Copied here by lambda-game's Build.bat so either repository can be cloned on its own; the
// version string is checked at load, so a stale copy is refused rather than run against.
#include "Game/LambdaGameAPI.h"

class ASourceEntity;

/**
 * The game module, loaded from LambdaGame.dll at startup.
 *
 * Everything that decides what an entity does lives on the far side of this; everything that draws, collides,
 * animates or makes noise stays here. The engine asks the DLL whether it implements a classname, and if it
 * does, the entity's behaviour comes from there instead of from a C++ class in this project.
 *
 * The DLL is optional. Without it every entity falls back to its native implementation and the game runs
 * exactly as it did before, which is what makes it safe to move entities across one at a time.
 */
class LAMBDASOURCE_API FLambdaGameDll : public lambda::IEngine
{
public:
	static FLambdaGameDll& Get();

	/** Loads the DLL beside the executable. Safe to call twice; returns whether a game is available. */
	bool Load();
	void Unload();
	bool IsLoaded() const { return Game != nullptr; }

	/** Whether the loaded game implements this classname. False when no DLL is loaded. */
	bool HandlesClass(const FString& ClassName) const;

	/**
	 * Makes the behaviour for one entity and gives it an id.
	 * The entity is registered so the DLL's calls can be resolved back to the actor.
	 */
	lambda::IEntity* CreateEntity(const FString& ClassName, AActor* Owner, lambda::EntityId& OutId);
	void DestroyEntity(lambda::IEntity* Behaviour, lambda::EntityId Id);

	// ---- lambda::IEngine, called from the DLL ----
	virtual const char* GetKeyValue(lambda::EntityId Entity, const char* Key) const override;
	virtual const char* GetClassName(lambda::EntityId Entity) const override;
	virtual const char* GetTargetName(lambda::EntityId Entity) const override;
	virtual void GetOrigin(lambda::EntityId Entity, lambda::Vec3* OutOrigin) const override;
	virtual void SetOrigin(lambda::EntityId Entity, const lambda::Vec3& Origin) override;
	virtual void GetAngles(lambda::EntityId Entity, lambda::Vec3* OutAngles) const override;
	virtual void SetAngles(lambda::EntityId Entity, const lambda::Vec3& Angles) override;
	virtual void GetBoundsSize(lambda::EntityId Entity, lambda::Vec3* OutSize) const override;
	virtual void LinearMove(lambda::EntityId Entity, const lambda::Vec3& Destination, float Speed) override;
	virtual void AngularMove(lambda::EntityId Entity, const lambda::Vec3& DestinationAngles, float Speed) override;
	virtual void AngularMoveAxis(lambda::EntityId Entity, const lambda::Vec3& AxisPoint, const lambda::Vec3& AxisDir,
		const lambda::Vec3& DestinationOrigin, const lambda::Vec3& DestinationAngles, float Speed) override;
	virtual void RotateAboutAxis(const lambda::Vec3& Origin, const lambda::Vec3& Angles, const lambda::Vec3& AxisPoint,
		const lambda::Vec3& AxisDir, float Degrees, lambda::Vec3* OutOrigin, lambda::Vec3* OutAngles) const override;
	virtual void StopMove(lambda::EntityId Entity) override;
	virtual void SetSolid(lambda::EntityId Entity, bool bSolid) override;
	virtual void SetSolidToPlayer(lambda::EntityId Entity, bool bSolid) override;
	virtual void SetCastShadows(lambda::EntityId Entity, bool bCast) override;
	virtual void SetTriggerVolume(lambda::EntityId Entity, bool bTrigger) override;
	virtual void SetLightScale(lambda::EntityId Entity, float Scale) override;
	virtual void EmitSound(lambda::EntityId Entity, const char* SoundNameOrPath) override;
	virtual void EmitSoundLooping(lambda::EntityId Entity, const char* SoundNameOrPath) override;
	virtual void StopLoopingSound(lambda::EntityId Entity) override;
	virtual void FireOutput(lambda::EntityId Entity, const char* OutputName, lambda::EntityId Activator) override;
	virtual bool NPCSetActivity(lambda::EntityId Entity, const char* Activity) override;
	virtual bool NPCActivityFinished(lambda::EntityId Entity) const override;
	virtual bool NPCMoveTo(lambda::EntityId Entity, const lambda::Vec3& Pos) override;
	virtual bool NPCMoveDone(lambda::EntityId Entity) const override;
	virtual void NPCStopMoving(lambda::EntityId Entity) override;
	virtual void NPCFaceToward(lambda::EntityId Entity, const lambda::Vec3& Pos) override;
	virtual bool NPCCanSee(lambda::EntityId Entity, lambda::EntityId Other, bool bIgnoreViewCone) const override;
	virtual bool NPCHasClearShot(lambda::EntityId Entity, lambda::EntityId Target) const override;
	virtual void NPCThrowGrenade(lambda::EntityId Entity, const lambda::Vec3& VelocityUnits, float FuseSeconds) override;
	virtual bool SolveGrenadeArc(lambda::EntityId Entity, const lambda::Vec3& TargetUnits, float FlightSeconds, lambda::Vec3* OutVelocityUnits) const override;
	virtual bool NPCFindGrenadeThreat(lambda::EntityId Entity, float RadiusUnits, lambda::Vec3* OutPosUnits, float* OutSecondsLeft) const override;
	virtual void NPCShootAt(lambda::EntityId Entity, lambda::EntityId Target, const lambda::NPCShotParams& Params) override;
	virtual void NPCShootAtPos(lambda::EntityId Entity, const lambda::Vec3& PosUnits, const lambda::NPCShotParams& Params) override;
	virtual bool NPCSpeak(lambda::EntityId Entity, const char* Soundscript) override;
	virtual bool NPCIsSpeaking(lambda::EntityId Entity) const override;
	virtual bool NPCFindCover(lambda::EntityId Entity, const lambda::Vec3& ThreatPosUnits, float MinDistUnits, float MaxDistUnits, lambda::Vec3* OutPosUnits) override;
	virtual bool NPCFindFlank(lambda::EntityId Entity, lambda::EntityId Target, float MinDistUnits, float MaxDistUnits, lambda::Vec3* OutPosUnits) override;
	virtual bool IsCoverFrom(const lambda::Vec3& PosUnits, const lambda::Vec3& ThreatPosUnits) const override;
	virtual float GetHealth(lambda::EntityId Entity) const override;
	virtual lambda::EntityId GetPlayer() const override;
	virtual float GetOutputMaxDelay(lambda::EntityId Entity, const char* OutputName) const override;
	virtual void CancelPendingOutputs(lambda::EntityId Entity) override;
	virtual void Remove(lambda::EntityId Entity) override;
	virtual bool IsPlayer(lambda::EntityId Entity) const override;
	virtual bool IsNPC(lambda::EntityId Entity) const override;
	virtual float GetTime() const override;
	virtual void Log(const char* Message) const override;

	/**
	 * The actor behind an id, or null once it has gone.
	 *
	 * AActor and not ASourceEntity, because a trigger has to be able to name what walked into it and the
	 * player is not a map entity - he has no keyvalues and no targetname, but he is certainly a toucher.
	 */
	AActor* ResolveEntity(lambda::EntityId Id) const;
	/** The same, for the calls that only make sense on something that came out of the map. */
	ASourceEntity* ResolveSourceEntity(lambda::EntityId Id) const;
	/** The id for an actor, allocating one if it does not have it yet. */
	lambda::EntityId IdForEntity(AActor* Entity);

private:
	FLambdaGameDll() = default;

	void* DllHandle = nullptr;
	lambda::IGame* Game = nullptr;

	TMap<uint32, TWeakObjectPtr<AActor>> EntitiesById;
	TMap<TWeakObjectPtr<AActor>, uint32> IdsByEntity;
	uint32 NextId = 1;

	/**
	 * Where the strings handed back to the DLL live.
	 *
	 * GetKeyValue returns a const char* into this rather than into an FString that is about to go out of
	 * scope. Documented as valid until the next call, and this is what makes that true.
	 */
	mutable TArray<char> ReturnedString;
	const char* StoreReturnString(const FString& Value) const;
};
