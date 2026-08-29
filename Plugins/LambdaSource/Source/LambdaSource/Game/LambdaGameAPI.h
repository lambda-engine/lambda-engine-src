// The contract between the engine and the game.
//
// Everything on the engine side of this header is Unreal; everything on the game side is plain C++ that a
// modder can build with nothing but a compiler. This file is the whole of what passes between them, which is
// why it is written the way it is:
//
//   * No Unreal types. The game DLL does not link Unreal, so FString and TArray cannot appear here.
//   * No std types either. A std::string crossing a DLL boundary depends on both sides being built with the
//     same compiler, runtime and flags; when they differ it does not fail loudly, it corrupts quietly. Source
//     pinned its compiler version for years over exactly this. Inside the DLL, std is fine and expected.
//   * Entities are opaque ids, not pointers. The engine's actors are garbage collected and may move; handing
//     the game a raw pointer would make its lifetime rules the game's problem.
//   * Pure virtual interfaces with no data members, so the layout is just a vtable and nothing about either
//     side's allocator or member ordering leaks across.
//
// The version string is checked at load. Change the interface, change the version, and an old DLL is refused
// with a clear message rather than crashing on a vtable that no longer means what it did.
#pragma once

// Bumped whenever anything below changes shape. A DLL built against an older one is refused at load.
#define LAMBDA_GAME_API_VERSION "LambdaGame004"

#if defined(_WIN32)
	#define LAMBDA_GAME_EXPORT extern "C" __declspec(dllexport)
#else
	#define LAMBDA_GAME_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace lambda
{

/** An entity, as far as the game is concerned. Zero is "none"; the engine owns what it refers to. */
typedef unsigned int EntityId;
static const EntityId InvalidEntity = 0;

/** A position or direction in Source units, Source's axes. No conversion happens across this boundary. */
struct Vec3
{
	float x = 0.0f, y = 0.0f, z = 0.0f;
};

/** Where a linear move finished, so a mover knows which end it arrived at. */
enum class MoveResult : int
{
	Arrived = 0,
	Interrupted = 1,
};

// ---------------------------------------------------------------------------------------------------------
// Implemented by the engine, called by the game.
// ---------------------------------------------------------------------------------------------------------

/**
 * What the game can ask the engine to do.
 *
 * Deliberately small. Rendering, animation, collision, navigation and audio mixing all stay on the engine
 * side; what crosses is the decision, not the machinery. A door does not draw itself - it says where it is
 * going and how fast, and the engine moves it.
 */
class IEngine
{
public:
	virtual ~IEngine() = default;

	// ---- the map ----
	/** A keyvalue as written in the map, or "" when absent. The pointer is valid until the next call. */
	virtual const char* GetKeyValue(EntityId Entity, const char* Key) const = 0;
	/** The entity's classname, e.g. "func_button". */
	virtual const char* GetClassName(EntityId Entity) const = 0;
	/** The entity's targetname, or "" when it has none. */
	virtual const char* GetTargetName(EntityId Entity) const = 0;

	// ---- placement ----
	virtual void GetOrigin(EntityId Entity, Vec3* OutOrigin) const = 0;
	virtual void SetOrigin(EntityId Entity, const Vec3& Origin) = 0;
	/** Pitch, yaw, roll in degrees - Source's QAngle, in Source's order. */
	virtual void GetAngles(EntityId Entity, Vec3* OutAngles) const = 0;
	virtual void SetAngles(EntityId Entity, const Vec3& Angles) = 0;
	/**
	 * The size of the entity's own bounding box, in Source units.
	 * A brush entity needs this to work out how far it can travel into its own thickness.
	 */
	virtual void GetBoundsSize(EntityId Entity, Vec3* OutSize) const = 0;

	// ---- movement ----
	/**
	 * Slides the entity to a position at a constant speed, calling OnMoveDone when it arrives.
	 *
	 * The engine owns the movement itself. That is the pragmatic half of the split: the game says where and
	 * how fast, and does not reimplement what the engine's movement already does well.
	 */
	virtual void LinearMove(EntityId Entity, const Vec3& Destination, float Speed) = 0;
	/** The same, turning instead of sliding: degrees per second toward a destination angle. */
	virtual void AngularMove(EntityId Entity, const Vec3& DestinationAngles, float Speed) = 0;
	virtual void StopMove(EntityId Entity) = 0;
	/** Whether the entity blocks anything. A door set passable stops shoving the player around. */
	virtual void SetSolid(EntityId Entity, bool bSolid) = 0;
	/**
	 * Turns the entity into a volume that things pass through and that reports what is inside it.
	 *
	 * A trigger is not a thin door: it never blocks, and it wants to be told when something enters and leaves
	 * rather than when something is in the way. Setting this replaces whatever solidity the entity had.
	 */
	virtual void SetTriggerVolume(EntityId Entity, bool bTrigger) = 0;

	// ---- output ----
	/** Plays a sound on the entity: a wav path under sound/, or a soundscript name. */
	virtual void EmitSound(EntityId Entity, const char* SoundNameOrPath) = 0;
	/**
	 * Starts a sound that loops until stopped - a door's travel noise, which has to end when the door does
	 * rather than after a fixed time. One per entity; starting another replaces it.
	 */
	virtual void EmitSoundLooping(EntityId Entity, const char* SoundNameOrPath) = 0;
	virtual void StopLoopingSound(EntityId Entity) = 0;
	/**
	 * Scales a light entity's brightness, 0 being off and 1 what the map asked for.
	 *
	 * The engine keeps the colour, the falloff and the units - everything that decides what the light looks
	 * like - and the game only says how much of it there should be right now. That is the whole of what a
	 * light appearance is: a pattern of numbers over time.
	 */
	virtual void SetLightScale(EntityId Entity, float Scale) = 0;

	/** Fires one of this entity's map outputs, e.g. "OnPressed". */
	virtual void FireOutput(EntityId Entity, const char* OutputName, EntityId Activator) = 0;

	// ---- what touched it ----
	/**
	 * Enough to answer a trigger's spawnflags without the game knowing what an Actor is.
	 *
	 * Only the two that matter in practice: nearly every trigger in a Half-Life map filters on Clients, and
	 * the rest on NPCs. The finer distinctions Source offers - physics debris, players in vehicles - would
	 * need the engine to answer questions it cannot yet, so a trigger set to those passes everything.
	 */
	virtual bool IsPlayer(EntityId Entity) const = 0;
	virtual bool IsNPC(EntityId Entity) const = 0;

	// ---- the world ----
	/** Seconds since the map started. */
	virtual float GetTime() const = 0;
	virtual void Log(const char* Message) const = 0;
};

// ---------------------------------------------------------------------------------------------------------
// Implemented by the game, called by the engine.
// ---------------------------------------------------------------------------------------------------------

/** One entity's behaviour. The engine owns the actor; this owns what it does. */
class IEntity
{
public:
	virtual ~IEntity() = default;

	/** After the keyvalues are readable and the geometry exists. */
	virtual void Spawn() = 0;
	/** Every frame. */
	virtual void Think(float DeltaSeconds) = 0;
	/** A LinearMove this entity asked for has finished. */
	virtual void OnMoveDone(MoveResult Result) = 0;

	/** The player pressed +USE while looking at it. */
	virtual void OnUse(EntityId Activator) = 0;
	/** Whether +USE should consider this entity at all. */
	virtual bool IsUsable() const = 0;

	/** A map input. Return false for one this entity does not implement. */
	virtual bool OnInput(const char* InputName, const char* Parameter, EntityId Activator) = 0;

	/** Something entered this entity's volume. Only sent to entities that asked to be one. */
	virtual void OnStartTouch(EntityId Other) = 0;
	/** Something left it. */
	virtual void OnEndTouch(EntityId Other) = 0;

	/**
	 * Something is in the way of a move this entity is making.
	 *
	 * Called while it stays blocked, not once - a door leaning on a player keeps hurting them, and deciding
	 * how often that happens is the game's business rather than the engine's.
	 */
	virtual void OnBlocked(EntityId Other) = 0;

	/** Called before the engine lets the entity go. */
	virtual void Destroy() = 0;
};

/** The game module as a whole. */
class IGame
{
public:
	virtual ~IGame() = default;

	/** Hands the game its engine. Return false to refuse to load. */
	virtual bool Init(IEngine* Engine) = 0;
	virtual void Shutdown() = 0;

	/** Whether the game implements this classname; the engine keeps its own for anything unclaimed. */
	virtual bool HandlesClass(const char* ClassName) const = 0;

	/** Makes the behaviour for one entity. Null means the engine should fall back to its own. */
	virtual IEntity* CreateEntity(const char* ClassName, EntityId Entity) = 0;
	virtual void DestroyEntity(IEntity* Behaviour) = 0;

	/** For the log line at startup, so it is obvious which game is loaded. */
	virtual const char* GetName() const = 0;
};

}	// namespace lambda

/**
 * The one exported symbol. The engine asks for an interface by version and gets null if this DLL does not
 * speak it - the same shape as Source's CreateInterface, and for the same reason.
 */
LAMBDA_GAME_EXPORT void* CreateLambdaGame(const char* RequestedVersion);
