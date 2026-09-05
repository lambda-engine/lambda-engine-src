#pragma once

#include "CoreMinimal.h"
#include "Particles/SourceParticleDefinition.h"

/**
 * The 24 fixed particle attribute slots (public/particles/particles.h). The integer index is what every
 * operator's "input field" / "output field" parameter refers to.
 */
enum class ESourceParticleAttr : uint8
{
	Xyz = 0,				// position (3)
	LifeDuration = 1,		// intended lifespan, seconds
	PrevXyz = 2,			// position last step; verlet velocity = (XYZ - PREV_XYZ) / dt (3)
	Radius = 3,
	Rotation = 4,			// roll, radians
	RotationSpeed = 5,		// radians/second
	TintRgb = 6,			// 0..1 per channel (3)
	Alpha = 7,				// 0..1
	CreationTime = 8,		// system-relative seconds
	SequenceNumber = 9,		// sheet sequence
	TrailLength = 10,		// seconds of movement the trail renderer draws
	ParticleId = 11,
	Yaw = 12,
	SequenceNumber1 = 13,
	HitboxIndex = 14,
	HitboxRelativeXyz = 15,	// (3)
	Alpha2 = 16,			// render alpha = ALPHA * ALPHA2
	ScratchVec = 17,		// (3)
	ScratchFloat = 18,
	Unused = 19,
	Pitch = 20,
	Normal = 21,			// (3), 0 0 0 = none
	GlowRgb = 22,			// (3)
	GlowAlpha = 23,
	Count = 24,
};

namespace SourceParticleAttr
{
	constexpr int32 Num = 24;
	/** Float components each slot occupies. */
	LAMBDASOURCE_API int32 Components(ESourceParticleAttr Attr);
	/** The engine's constant-block default for an attribute nothing writes. */
	LAMBDASOURCE_API float DefaultComponent(ESourceParticleAttr Attr, int32 Component);
	/** An "output field" integer as an attribute, guarding bad indices with the operator's own default. */
	LAMBDASOURCE_API ESourceParticleAttr FromField(int32 Field, ESourceParticleAttr Fallback);
}

/**
 * Structure-of-arrays particle storage. Each attribute is either allocated per particle (once anything writes
 * it) or read from a constant block seeded by the definition. Keeps an initial-value snapshot for the "output
 * is scalar of initial random range" options, and compacts kills preserving creation order, as the engine does.
 */
class LAMBDASOURCE_API FSourceParticleCollection
{
public:
	explicit FSourceParticleCollection(int32 MaxParticles);

	int32 Capacity() const { return MaxCount; }
	int32 Num() const { return Count; }

	void SetConstant(ESourceParticleAttr Attr, int32 Component, float Value);
	float GetConstant(ESourceParticleAttr Attr, int32 Component) const;

	/** Spawns up to Requested particles (clamped to what is free), returning the first index of the new range. */
	int32 Spawn(int32 Requested, int32& OutSpawned);

	int32 IdOf(int32 Particle) const { return Ids[Particle]; }

	float GetFloat(ESourceParticleAttr Attr, int32 Particle, int32 Component = 0) const;
	void SetFloat(ESourceParticleAttr Attr, int32 Particle, float Value, int32 Component = 0);
	FVector3f GetVector(ESourceParticleAttr Attr, int32 Particle) const;
	void SetVector(ESourceParticleAttr Attr, int32 Particle, const FVector3f& Value);

	/** Snapshots [Start, Start+Count) of the given attributes as their initial values. */
	void SnapshotInitial(int32 Start, int32 RangeCount, TConstArrayView<ESourceParticleAttr> Attrs);
	float GetInitialFloat(ESourceParticleAttr Attr, int32 Particle, int32 Component = 0) const;
	FVector3f GetInitialVector(ESourceParticleAttr Attr, int32 Particle) const;

	void Kill(int32 Particle) { Killed[Particle] = 1; }
	void KillAll();
	bool IsKilled(int32 Particle) const { return Killed[Particle] != 0; }
	/** Removes killed particles keeping the rest in creation order. Returns how many went. */
	int32 ApplyKills();

private:
	void Allocate(ESourceParticleAttr Attr);

	int32 MaxCount = 0;
	int32 Count = 0;
	int32 NextId = 0;
	TArray<float> Data[SourceParticleAttr::Num];		// empty = read the constant block
	TArray<float> Initial[SourceParticleAttr::Num];
	float Constants[SourceParticleAttr::Num * 3] = {};
	TArray<int32> Ids;
	TArray<uint8> Killed;
};

/** One of the 64 control points: a position with its previous position, an orientation basis, and a parent. */
struct LAMBDASOURCE_API FSourceParticleControlPoint
{
	FVector3f Position = FVector3f::ZeroVector;
	FVector3f PreviousPosition = FVector3f::ZeroVector;
	// The identity basis, so a "local space" vector with no orientation set is the vector itself.
	FVector3f Forward = FVector3f(0, 1, 0);
	FVector3f Up = FVector3f(0, 0, 1);
	FVector3f Right = FVector3f(1, 0, 0);
	int32 Parent = -1;

	FVector3f Velocity(float Dt) const { return Dt > 0.0f ? (Position - PreviousPosition) / Dt : FVector3f::ZeroVector; }
	/** TransformAxis: x*right + y*forward + z*up. */
	FVector3f TransformLocal(const FVector3f& Local) const { return Local.X * Right + Local.Y * Forward + Local.Z * Up; }

	/** Orients from Source angles (pitch yaw roll, degrees) the way AngleVectors does. */
	void SetOrientationFromAngles(const FVector3f& Angles);
};

/**
 * The engine's deterministic random source: a fixed 4096-entry table indexed by (seed + sample) & 4095. The
 * values are not Valve's, so playback is stable and reproducible but not bit-identical to the game.
 */
class LAMBDASOURCE_API FSourceParticleRandom
{
public:
	static constexpr int32 Size = 4096;
	static constexpr int32 Mask = Size - 1;
	/** The per-operator sample-offset stride (m_nOperatorRandomSampleOffset). */
	static constexpr int32 OperatorOffsetStride = 17;

	static float At(int32 Index);
	static float Float(int32 Seed, int32 Sample) { return At(Seed + Sample); }
	static float Range(int32 Seed, int32 Sample, float Min, float Max) { return Min + Float(Seed, Sample) * (Max - Min); }
	static float RangeExp(int32 Seed, int32 Sample, float Min, float Max, float Exponent);
};

namespace SourceParticleMath
{
	constexpr float DegToRad = PI / 180.0f;

	inline float Lerp(float A, float B, float T) { return A + (B - A) * T; }
	inline float Clamp01(float T) { return T < 0.0f ? 0.0f : (T > 1.0f ? 1.0f : T); }
	/** [InMin, InMax] -> [0, 1], clamped. */
	float RemapClamped(float Value, float InMin, float InMax);
	/** mathlib Bias: reshapes T so Bias(0.5, B) == B. */
	float Bias(float T, float B);
	/** mathlib SimpleSpline: 3t^2 - 2t^3. */
	inline float SimpleSpline(float T) { return T * T * (3.0f - 2.0f * T); }
	/** The fade curve the eased operators use: optional bias reshape, then the spline when easing is on. */
	float FadeCurve(float T, bool bEaseInAndOut, float BiasValue);
	/** Per-step drag factor: Drag is the velocity lost per 1/30 s, re-normalised to Dt. */
	float DragAdjusted(float Drag, float Dt);
	/** Deterministic 1-D value noise in about [-1, 1], hermite-blended between random lattice values. */
	float Noise1D(float Coordinate);
	/** 3-D value noise, one decorrelated field per axis. */
	FVector3f Noise3D(const FVector3f& Coordinate);
	/** Deterministic random unit vector for a seed/sample pair. */
	FVector3f RandomUnitVector(int32 Seed, int32 Sample);
	/** mathlib VectorVectors: an orthonormal right/up for a forward vector. */
	void VectorVectors(const FVector3f& Forward, FVector3f& OutRight, FVector3f& OutUp);
	/** Rotates V about a unit axis by an angle in radians. */
	FVector3f RotateAboutAxis(const FVector3f& V, const FVector3f& UnitAxis, float Radians);
}

struct FSourceParticleTraceHit
{
	bool bHit = false;
	FVector3f Position = FVector3f::ZeroVector;		// Source units
	FVector3f Normal = FVector3f(0, 0, 1);
	float Fraction = 1.0f;
};

class FSourceParticleSimulator;
class FSourceParticleOp;

/** A child system running under a parent: the link and the simulator it drives. */
struct LAMBDASOURCE_API FSourceParticleChildSim
{
	FSourceParticleChildDef Link;
	TUniquePtr<FSourceParticleSimulator> Simulator;
	bool bStarted = false;
	/** Control points a child-CP operator wrote this frame; the parent's copy skips them so the write survives. */
	TSet<int32> OverriddenControlPoints;

	FSourceParticleChildSim();
	~FSourceParticleChildSim();
	FSourceParticleChildSim(FSourceParticleChildSim&&);
	FSourceParticleChildSim& operator=(FSourceParticleChildSim&&);
};

/**
 * What every operator sees while it runs: the control points, the clock, the end-cap and emission state, the
 * random offset, the parent's particles, the children, and the hooks into the world (traces, the player) that
 * the actor supplies. Everything is in Source units.
 */
struct LAMBDASOURCE_API FSourceParticleContext
{
	static constexpr int32 MaxControlPoints = 64;

	FSourceParticleControlPoint ControlPoints[MaxControlPoints];

	float Time = 0.0f;					// system-relative seconds
	float DeltaTime = 0.0f;				// the current sub-step
	float PreviousDeltaTime = 0.0f;		// the sub-step before it (m_flPreviousDt), for rescaling verlet deltas
	bool bInEndCap = false;
	bool bEmissionStopped = false;
	int32 Seed = 0;
	int32 OperatorRandomOffset = 0;

	const FSourceParticleCollection* ParentParticles = nullptr;
	TArray<FSourceParticleChildSim>* Children = nullptr;
	const TArray<TUniquePtr<FSourceParticleOp>>* Forces = nullptr;
	const TArray<TUniquePtr<FSourceParticleOp>>* Constraints = nullptr;

	/** Line trace against the world, Source space; unset when the system runs without a world (previews). */
	TFunction<bool(const FVector3f& Start, const FVector3f& End, FSourceParticleTraceHit& OutHit)> TraceLine;
	/** The local player, for the operators that want him; bHavePlayer says whether the actor filled it in. */
	bool bHavePlayer = false;
	FVector3f PlayerPosition = FVector3f::ZeroVector;
	FVector3f PlayerEyeAngles = FVector3f::ZeroVector;
	/** Frames in each sheet sequence of the system's material, for Lifetime From Sequence. Empty = no sheet. */
	TArray<int32> SheetSequenceFrameCounts;

	int32 ClampControlPoint(int32 Index) const { return FMath::Clamp(Index, 0, MaxControlPoints - 1); }
	const FSourceParticleControlPoint& CP(int32 Index) const { return ControlPoints[ClampControlPoint(Index)]; }
	FSourceParticleControlPoint& CP(int32 Index) { return ControlPoints[ClampControlPoint(Index)]; }
	/** One X/Y/Z component of a control point's position, the "field" reading many operators take. */
	float CPField(int32 Index, int32 Field) const;

	int32 AdvanceOperatorOffset()
	{
		const int32 Current = OperatorRandomOffset;
		OperatorRandomOffset += FSourceParticleRandom::OperatorOffsetStride;
		return Current;
	}
};

/**
 * Base of every simulated operator. One instance per operator per system, so per-emitter state lives here. The
 * subclass reads its parameters from the DMX element once, in Configure(). The strength envelope shared by all
 * operators (BEGIN_PARTICLE_OPERATOR_UNPACK) is read here.
 */
class LAMBDASOURCE_API FSourceParticleOp
{
public:
	virtual ~FSourceParticleOp() {}

	void Init(const FSourceDMXElement* InElement, int32 InInstanceSeed);

	virtual bool RunBeforeEmitters() const { return false; }
	virtual bool IsImplemented() const { return true; }
	virtual void Reset() {}

	virtual void Emit(FSourceParticleCollection& Particles, FSourceParticleContext& Ctx, float Strength) {}
	virtual void InitNewParticles(FSourceParticleCollection& Particles, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) {}
	virtual void Operate(FSourceParticleCollection& Particles, FSourceParticleContext& Ctx, float Strength) {}
	virtual void AddForces(FSourceParticleCollection& Particles, FSourceParticleContext& Ctx, float Strength, TArray<FVector3f>& Accelerations) {}
	virtual bool IsFinalConstraint() const { return false; }
	virtual bool ApplyConstraint(FSourceParticleCollection& Particles, FSourceParticleContext& Ctx) { return false; }

	/** This operator's strength for the current step; 0 means it is skipped. */
	float Strength(const FSourceParticleContext& Ctx) const;

	const FSourceDMXElement* GetElement() const { return Element; }
	int32 GetInstanceSeed() const { return InstanceSeed; }

protected:
	/** Reads the operator's own parameters; called once after Init. */
	virtual void Configure() {}

	// Parameter reads, so subclasses stay one line per field.
	float F(const TCHAR* Key, float Default) const { return Element ? Element->GetFloat(Key, Default) : Default; }
	int32 I(const TCHAR* Key, int32 Default) const { return Element ? Element->GetInt(Key, Default) : Default; }
	bool B(const TCHAR* Key, bool bDefault) const { return Element ? Element->GetBool(Key, bDefault) : bDefault; }
	FVector3f V(const TCHAR* Key, const FVector3f& Default) const { return Element ? Element->GetVector3(Key, Default) : Default; }
	FVector4f V4(const TCHAR* Key, const FVector4f& Default) const { return Element ? Element->GetVector4(Key, Default) : Default; }
	FColor C(const TCHAR* Key, const FColor& Default) const { return Element ? Element->GetColor(Key, Default) : Default; }
	FString S(const TCHAR* Key, const FString& Default) const { return Element ? Element->GetString(Key, Default) : Default; }

	// Per-particle randoms. Stable draws index by the particle id alone (fade windows, cull thresholds - the
	// same value every frame); per-frame draws add the advancing operator offset.
	float StableRand(const FSourceParticleCollection& P, int32 Particle, int32 Salt) const
	{
		return FSourceParticleRandom::Float(InstanceSeed * 31 + Salt, P.IdOf(Particle));
	}
	float StableRandRangeExp(const FSourceParticleCollection& P, int32 Particle, int32 Salt, float Min, float Max, float Exponent) const
	{
		return FSourceParticleRandom::RangeExp(InstanceSeed * 31 + Salt, P.IdOf(Particle), Min, Max, Exponent);
	}
	float FrameRand(const FSourceParticleCollection& P, const FSourceParticleContext& Ctx, int32 Particle, int32 Salt) const
	{
		return FSourceParticleRandom::Float(InstanceSeed * 31 + Salt, P.IdOf(Particle) + Ctx.OperatorRandomOffset);
	}
	float FrameRandRange(const FSourceParticleCollection& P, const FSourceParticleContext& Ctx, int32 Particle, int32 Salt, float Min, float Max) const
	{
		return Min + FrameRand(P, Ctx, Particle, Salt) * (Max - Min);
	}
	float FrameRandRangeExp(const FSourceParticleCollection& P, const FSourceParticleContext& Ctx, int32 Particle, int32 Salt, float Min, float Max, float Exponent) const
	{
		return FSourceParticleRandom::RangeExp(InstanceSeed * 31 + Salt, P.IdOf(Particle) + Ctx.OperatorRandomOffset, Min, Max, Exponent);
	}
	FVector3f FrameRandVector(const FSourceParticleCollection& P, const FSourceParticleContext& Ctx, int32 Particle, int32 Salt, const FVector3f& Min, const FVector3f& Max) const
	{
		return FVector3f(FrameRandRange(P, Ctx, Particle, Salt, Min.X, Max.X), FrameRandRange(P, Ctx, Particle, Salt + 1, Min.Y, Max.Y),
			FrameRandRange(P, Ctx, Particle, Salt + 2, Min.Z, Max.Z));
	}
	FVector3f FrameRandUnit(const FSourceParticleCollection& P, const FSourceParticleContext& Ctx, int32 Particle, int32 Salt) const
	{
		return SourceParticleMath::RandomUnitVector(InstanceSeed * 31 + Salt, P.IdOf(Particle) + Ctx.OperatorRandomOffset);
	}

	static float Age(const FSourceParticleCollection& P, const FSourceParticleContext& Ctx, int32 Particle)
	{
		return Ctx.Time - P.GetFloat(ESourceParticleAttr::CreationTime, Particle);
	}
	static float LifeFraction(const FSourceParticleCollection& P, const FSourceParticleContext& Ctx, int32 Particle)
	{
		const float Life = P.GetFloat(ESourceParticleAttr::LifeDuration, Particle);
		return Life > 0.0f ? Age(P, Ctx, Particle) / Life : 1.0f;
	}
	/**
	 * Gives a new particle a velocity by offsetting PREV_XYZ (verlet). The offset is over the previous step, as
	 * the engine's initializers write it, because Movement Basic rescales every delta by dt / previous dt.
	 */
	static void AddVelocity(FSourceParticleCollection& P, const FSourceParticleContext& Ctx, int32 Particle, const FVector3f& Velocity)
	{
		const float Dt = Ctx.PreviousDeltaTime > 0.0f ? Ctx.PreviousDeltaTime : (Ctx.DeltaTime > 0.0f ? Ctx.DeltaTime : (1.0f / 60.0f));
		P.SetVector(ESourceParticleAttr::PrevXyz, Particle, P.GetVector(ESourceParticleAttr::PrevXyz, Particle) - Velocity * Dt);
	}

	const FSourceDMXElement* Element = nullptr;
	int32 InstanceSeed = 0;

	// The envelope.
	float OpStartFadeIn = 0.0f, OpEndFadeIn = 0.0f, OpStartFadeOut = 0.0f, OpEndFadeOut = 0.0f, OpFadeOscillate = 0.0f;
	int32 OpTimeOffsetSeed = 0;
	float OpTimeOffsetMin = 0.0f, OpTimeOffsetMax = 0.0f;
	int32 OpTimeScaleSeed = 0;
	float OpTimeScaleMin = 1.0f, OpTimeScaleMax = 1.0f;
	int32 OpStrengthScaleSeed = 0;
	float OpStrengthMin = 1.0f, OpStrengthMax = 1.0f;
	int32 OpEndCapState = -1;
};

/** Creates the simulated operator for a function name, or a placeholder that does nothing (and says so once). */
namespace SourceParticleOps
{
	LAMBDASOURCE_API TUniquePtr<FSourceParticleOp> Create(const TCHAR* Category, const FString& FunctionName,
		const FSourceDMXElement* Element, int32 InstanceSeed);
	LAMBDASOURCE_API bool IsImplemented(const TCHAR* Category, const FString& FunctionName);
}

/**
 * CParticleCollection::Simulate for one definition: per sub-step (dt clamped to "maximum time step", at most
 * 10 sub-steps) the pre-emitter operators, the emitters, the initializers over the new range, then the operator
 * list in file order with the kill list applied after each. Movement Basic runs the forces and the constraints
 * from inside. Children simulate after their delay, reading a copy of the parent's control points.
 */
class LAMBDASOURCE_API FSourceParticleSimulator
{
public:
	explicit FSourceParticleSimulator(TSharedPtr<const FSourceParticleDefinition> InDefinition);
	~FSourceParticleSimulator();

	/** Fresh storage and per-operator state; control points keep their values. */
	void Reset();
	/** Advances by a frame. */
	void Update(float DeltaTime);
	/** StopEmission: no more particles from any emitter, this system and its children; the rest die naturally. */
	void StopEmission(bool bPlayEndCap);
	/** Everything, now. */
	void KillAllParticles();

	/** Applies world hooks and the player to this system and every child. */
	void SetWorldHooks(const FSourceParticleContext& Template);
	/** Copies a control point into this system and every child (SetControlPoint propagates down). */
	void SetControlPoint(int32 Index, const FVector3f& Position, bool bResetPrevious);
	void SetControlPointOrientation(int32 Index, const FVector3f& Forward, const FVector3f& Right, const FVector3f& Up);

	/** The system time past which every emitter, here and below, has finished; +inf for endless effects. */
	float ComputeEmittersEndTime(float Delay = 0.0f) const;
	int32 TotalParticleCount() const;
	bool IsEmissionStopped() const { return Context.bEmissionStopped; }

	const FSourceParticleDefinition& GetDefinition() const { return *Definition; }
	FSourceParticleCollection& GetParticles() { return *Particles; }
	const FSourceParticleCollection& GetParticles() const { return *Particles; }
	FSourceParticleContext& GetContext() { return Context; }
	const FSourceParticleContext& GetContext() const { return Context; }
	TArray<FSourceParticleChildSim>& GetChildren() { return Children; }
	const TArray<FSourceParticleChildSim>& GetChildren() const { return Children; }
	const TArray<TUniquePtr<FSourceParticleOp>>& GetRenderers() const { return Renderers; }

	/** Visits this system and then every descendant, parents before children. */
	void ForEachSystem(TFunctionRef<void(FSourceParticleSimulator&)> Visit);

private:
	FSourceParticleSimulator(TSharedPtr<const FSourceParticleDefinition> InDefinition, TSet<const FSourceParticleDefinition*>& Ancestors);
	void Construct(TSet<const FSourceParticleDefinition*>& Ancestors);
	void BuildOps(const TArray<FSourceParticleOperatorDef>& Defs, const TCHAR* Category, TArray<TUniquePtr<FSourceParticleOp>>& Out, int32 SeedBase);
	void SeedConstants();
	void SimulateFirstFrame();
	void Substep(float StepDt);
	void RunPreEmitterOperators();
	void InitializeNewParticles(int32 Start, int32 Count);
	void UpdateChild(FSourceParticleChildSim& Child, float DeltaTime);

	TSharedPtr<const FSourceParticleDefinition> Definition;
	TUniquePtr<FSourceParticleCollection> Particles;
	FSourceParticleContext Context;
	TArray<TUniquePtr<FSourceParticleOp>> Emitters;
	TArray<TUniquePtr<FSourceParticleOp>> Initializers;
	TArray<TUniquePtr<FSourceParticleOp>> Operators;
	TArray<TUniquePtr<FSourceParticleOp>> Forces;
	TArray<TUniquePtr<FSourceParticleOp>> Constraints;
	TArray<TUniquePtr<FSourceParticleOp>> Renderers;
	TArray<FSourceParticleChildSim> Children;
	bool bFirstFrameDone = false;
};
