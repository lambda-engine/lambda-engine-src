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
#define LAMBDA_GAME_API_VERSION "LambdaGame009"

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

/** One trigger pull, as the engine needs to hear it. Plain data, like everything crossing this boundary. */
struct NPCShotParams
{
	int Pellets;					// a shotgun is one pull, many pellets
	float SpreadDegrees;			// full cone angle
	float DamagePerPellet;			// skill.cfg's number, passed through
	const char* FireSound;			// soundscript for the muzzle report
};

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
	/**
	 * The same again, but swinging about a hinge rather than walking the three angles independently.
	 *
	 * AngularMove moves each of pitch, yaw and roll towards its destination on its own. That traces the swing
	 * of a hinge only when the hinge runs through the entity's own origin along one of the three axes; on any
	 * other the entity arrives in the right place having visibly wobbled its way there. This turns about the
	 * hinge, so any hinge swings like a hinge.
	 *
	 * The hinge is a line: AxisPoint is somewhere on it and AxisDir is the way it runs (Source space, and it
	 * need not be normalised). A line that misses the entity's origin carries the entity around it, so the
	 * destination is a whole pose - where it ends up as well as which way it ends up facing.
	 *
	 * How far is left to go is measured from wherever the entity is now, so a door reversed half way through
	 * swings back through the part it actually travelled. Swings of more than half a turn are not
	 * expressible: the short way round is always the way taken.
	 */
	virtual void AngularMoveAxis(EntityId Entity, const Vec3& AxisPoint, const Vec3& AxisDir,
		const Vec3& DestinationOrigin, const Vec3& DestinationAngles, float Speed) = 0;
	/**
	 * Where turning Degrees about a hinge puts an entity - the question "where does this hinge put the door
	 * when it is open?".
	 *
	 * A query, not a movement: nothing is moved and nothing is remembered. It is here rather than in the game
	 * because composing two rotations wants a quaternion, and the engine already has one.
	 */
	virtual void RotateAboutAxis(const Vec3& Origin, const Vec3& Angles, const Vec3& AxisPoint,
		const Vec3& AxisDir, float Degrees, Vec3* OutOrigin, Vec3* OutAngles) const = 0;
	virtual void StopMove(EntityId Entity) = 0;
	/** Whether the entity blocks anything. A door set passable stops shoving the player around. */
	virtual void SetSolid(EntityId Entity, bool bSolid) = 0;
	/**
	 * Whether the entity blocks the player in particular, leaving it solid to everything else.
	 *
	 * The difference from SetSolid is who gets through: a passable door is scenery that nothing
	 * collides with, while one that is only non-solid to the player still shuts NPCs out and still
	 * stops the crates you throw at it.
	 */
	virtual void SetSolidToPlayer(EntityId Entity, bool bSolid) = 0;
	/** Whether the entity casts a shadow. Turned off, light passes through it as though it were not there. */
	virtual void SetCastShadows(EntityId Entity, bool bCast) = 0;
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

	/**
	 * The longest delay any connection on this output was given, or -1 if nothing is connected to it.
	 *
	 * COutputEvent::GetMaxDelay. An entity that must not fire again until its last output has gone out has to
	 * know how long that is, and the connections are the engine's - they were parsed out of the map.
	 *
	 * -1 rather than 0 for an unconnected output, because "nothing is listening" and "everything is listening
	 * right now" are different questions and both get asked: a logic_relay only announces its own spawn when
	 * a map actually wired something to it.
	 */
	virtual float GetOutputMaxDelay(EntityId Entity, const char* OutputName) const = 0;

	/**
	 * Drops every event this entity has fired that has not gone out yet (CEventQueue::CancelEvents).
	 *
	 * Only the ones it fired itself. Events on their way to it are somebody else's to cancel.
	 */
	virtual void CancelPendingOutputs(EntityId Entity) = 0;

	/**
	 * Takes the entity out of the map (UTIL_Remove).
	 *
	 * Deferred, not immediate: an entity usually asks for this from inside an input it is still handling, and
	 * it has to survive returning from that. Outputs it has already fired still go out - the queue holds
	 * them, not the entity that queued them.
	 */
	virtual void Remove(EntityId Entity) = 0;

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

	// ---- an NPC's body ----
	//
	// The mind lives in this DLL; the body - model, animation, navigation, traces - is the engine's. These
	// are the orders a mind gives its body and the questions it asks of its senses, all in Source units.
	// They mean nothing for entities that are not NPCs.

	/** Plays the sequence the model has for an activity ("ACT_RUN"). False if the model has none for it. */
	virtual bool NPCSetActivity(EntityId Entity, const char* Activity) = 0;
	/** Whether a non-looping activity has finished playing. */
	virtual bool NPCActivityFinished(EntityId Entity) const = 0;

	/**
	 * Heads for a place through the navmesh. The route is the engine's; deciding where to go is the mind's.
	 * False when there is no route at all - the mind should pick somewhere else, not wait.
	 */
	virtual bool NPCMoveTo(EntityId Entity, const Vec3& Pos) = 0;
	/** Arrived, failed, or got stuck - the move is over either way. Where it ended up says which. */
	virtual bool NPCMoveDone(EntityId Entity) const = 0;
	virtual void NPCStopMoving(EntityId Entity) = 0;
	/** Turns toward a point at the body's own yaw speed. */
	virtual void NPCFaceToward(EntityId Entity, const Vec3& Pos) = 0;

	/**
	 * The most interesting noise this NPC can currently hear (CSoundEnt), or false for silence.
	 *
	 * Sounds are things that happened at places, not messages sent to listeners: a shot, a round striking a
	 * wall, somebody walking. Whether this NPC noticed depends on how far the noise carries and where it is
	 * standing, which is what stops an AI reacting to a gunshot it could not possibly have heard.
	 *
	 * OutIsCombat separates violence from footsteps - both are worth turning round for, only one is worth
	 * shouting about.
	 */
	virtual bool NPCHearSound(EntityId Entity, Vec3* OutPosUnits, bool* OutIsCombat) const = 0;

	/** A clear line from this NPC's eyes to the other's, and (unless ignored) within its view cone. */
	virtual bool NPCCanSee(EntityId Entity, EntityId Other, bool bIgnoreViewCone) const = 0;

	/**
	 * Whether a shot at the target would go past this NPC's own side (CAI_BaseNPC::PointInSpread, which
	 * Source uses for exactly this). False means somebody friendly is in the line of fire and the trigger
	 * should not be pulled - holding fire while a squadmate crosses is most of what a squad looks like.
	 */
	virtual bool NPCHasClearShot(EntityId Entity, EntityId Target) const = 0;

	/**
	 * One trigger pull. The engine traces the pellets from the eyes toward the target with the given spread,
	 * lands the damage and plays the fire sound; the mind owns the clip, the rate and the bursts.
	 */
	virtual void NPCShootAt(EntityId Entity, EntityId Target, const NPCShotParams& Params) = 0;
	/**
	 * The same trigger pull aimed at a position - suppression: shooting at where somebody was, so they hear
	 * it and stay down, without needing to see them.
	 */
	virtual void NPCShootAtPos(EntityId Entity, const Vec3& PosUnits, const NPCShotParams& Params) = 0;

	/**
	 * Throws a grenade with the given velocity in Source units per second, arming it for FuseSeconds.
	 *
	 * The arc is the thrower's business: an NPC that wants one to land somewhere works out the velocity that
	 * gets it there (SolveGrenadeArc) and hands it over. The engine flies it, bounces it off the world and
	 * blows it up.
	 */
	virtual void NPCThrowGrenade(EntityId Entity, const Vec3& VelocityUnits, float FuseSeconds) = 0;

	/**
	 * The velocity that would lob a grenade from this NPC's hand onto a spot in FlightSeconds, or false if
	 * no throw an arm could make gets there. Ballistics belong on this side of the boundary because the
	 * engine owns gravity.
	 *
	 * Flight time is not the fuse: Source throws a grenade so it arrives quickly and lets it lie there
	 * cooking. Solving for the whole fuse instead asks for a near-vertical lob nobody could throw.
	 */
	virtual bool SolveGrenadeArc(EntityId Entity, const Vec3& TargetUnits, float FlightSeconds, Vec3* OutVelocityUnits) const = 0;

	/**
	 * A live grenade somebody else threw, within the radius - and how long is left on it.
	 *
	 * This is a thing noticed in the world rather than a message sent to a victim, which is the difference
	 * between an AI that dives because it saw a grenade and one that dives because it was told to.
	 */
	virtual bool NPCFindGrenadeThreat(EntityId Entity, float RadiusUnits, Vec3* OutPosUnits, float* OutSecondsLeft) const = 0;

	/** Says a soundscript line, cutting whatever it was saying. False if the entity cannot speak. */
	virtual bool NPCSpeak(EntityId Entity, const char* Soundscript) = 0;
	virtual bool NPCIsSpeaking(EntityId Entity) const = 0;

	/**
	 * Tactics read from the level itself, not placed by a mapper. A cover point is somewhere reachable whose
	 * chest-height line to the threat is blocked by the world; a flank point is somewhere reachable that can
	 * see the threat from a meaningfully different side than this NPC is on now. Both are computed live from
	 * the geometry, so a threat that moves invalidates yesterday's answer - which is what IsCoverFrom is for.
	 */
	virtual bool NPCFindCover(EntityId Entity, const Vec3& ThreatPosUnits, float MinDistUnits, float MaxDistUnits, Vec3* OutPosUnits) = 0;
	virtual bool NPCFindFlank(EntityId Entity, EntityId Target, float MinDistUnits, float MaxDistUnits, Vec3* OutPosUnits) = 0;
	virtual bool IsCoverFrom(const Vec3& PosUnits, const Vec3& ThreatPosUnits) const = 0;

	virtual float GetHealth(EntityId Entity) const = 0;
	/** The player, or InvalidEntity before one exists. The only enemy an NPC has, for now. */
	virtual EntityId GetPlayer() const = 0;

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

	/**
	 * This entity was hurt. What a mind does with pain is its own affair: flinch, bark, abandon the plan
	 * that led here. The body already took the damage - this is news, not a request.
	 */
	virtual void OnDamaged(EntityId Attacker, float Amount) = 0;

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
