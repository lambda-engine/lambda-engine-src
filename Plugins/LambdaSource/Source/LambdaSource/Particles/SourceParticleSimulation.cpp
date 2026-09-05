#include "Particles/SourceParticleSimulation.h"
#include "Core/LambdaSourceModule.h"

// ---- Attributes -------------------------------------------------------------------------------------------------

namespace SourceParticleAttr
{
	static const int32 GComponents[Num] = { 3, 1, 3, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 1, 1, 3, 1, 3, 1, 1, 1, 3, 3, 1 };

	int32 Components(ESourceParticleAttr Attr)
	{
		return GComponents[(int32)Attr];
	}

	float DefaultComponent(ESourceParticleAttr Attr, int32 Component)
	{
		switch (Attr)
		{
		case ESourceParticleAttr::Radius: return 5.0f;
		case ESourceParticleAttr::TrailLength: return 0.1f;
		case ESourceParticleAttr::TintRgb:
		case ESourceParticleAttr::Alpha:
		case ESourceParticleAttr::Alpha2: return 1.0f;
		case ESourceParticleAttr::Normal: return Component == 2 ? 1.0f : 0.0f;
		default: return 0.0f;
		}
	}

	ESourceParticleAttr FromField(int32 Field, ESourceParticleAttr Fallback)
	{
		return (Field >= 0 && Field < Num) ? (ESourceParticleAttr)Field : Fallback;
	}
}

// ---- FSourceParticleCollection -----------------------------------------------------------------------------------

FSourceParticleCollection::FSourceParticleCollection(int32 MaxParticles)
	: MaxCount(FMath::Max(1, MaxParticles))
{
	Ids.SetNumZeroed(MaxCount);
	Killed.SetNumZeroed(MaxCount);
	for (int32 A = 0; A < SourceParticleAttr::Num; ++A)
	{
		for (int32 C = 0; C < SourceParticleAttr::Components((ESourceParticleAttr)A); ++C)
		{
			Constants[A * 3 + C] = SourceParticleAttr::DefaultComponent((ESourceParticleAttr)A, C);
		}
	}
}

void FSourceParticleCollection::SetConstant(ESourceParticleAttr Attr, int32 Component, float Value)
{
	Constants[(int32)Attr * 3 + Component] = Value;
}

float FSourceParticleCollection::GetConstant(ESourceParticleAttr Attr, int32 Component) const
{
	return Constants[(int32)Attr * 3 + Component];
}

void FSourceParticleCollection::Allocate(ESourceParticleAttr Attr)
{
	const int32 A = (int32)Attr;
	if (Data[A].Num() > 0)
	{
		return;
	}
	const int32 Comps = SourceParticleAttr::Components(Attr);
	Data[A].SetNumUninitialized(MaxCount * Comps);
	// Existing particles read what the constant block said until now.
	for (int32 P = 0; P < MaxCount; ++P)
	{
		for (int32 C = 0; C < Comps; ++C)
		{
			Data[A][P * Comps + C] = Constants[A * 3 + C];
		}
	}
}

int32 FSourceParticleCollection::Spawn(int32 Requested, int32& OutSpawned)
{
	const int32 Start = Count;
	OutSpawned = FMath::Max(0, FMath::Min(Requested, MaxCount - Count));
	for (int32 i = 0; i < OutSpawned; ++i)
	{
		const int32 P = Start + i;
		Ids[P] = NextId++;
		Killed[P] = 0;
		for (int32 A = 0; A < SourceParticleAttr::Num; ++A)
		{
			if (Data[A].Num() == 0)
			{
				continue;
			}
			const int32 Comps = SourceParticleAttr::Components((ESourceParticleAttr)A);
			for (int32 C = 0; C < Comps; ++C)
			{
				Data[A][P * Comps + C] = Constants[A * 3 + C];
			}
		}
	}
	Count += OutSpawned;
	return Start;
}

float FSourceParticleCollection::GetFloat(ESourceParticleAttr Attr, int32 Particle, int32 Component) const
{
	const int32 A = (int32)Attr;
	if (Data[A].Num() == 0)
	{
		return Constants[A * 3 + Component];
	}
	return Data[A][Particle * SourceParticleAttr::Components(Attr) + Component];
}

void FSourceParticleCollection::SetFloat(ESourceParticleAttr Attr, int32 Particle, float Value, int32 Component)
{
	const int32 A = (int32)Attr;
	if (Data[A].Num() == 0)
	{
		Allocate(Attr);		// the first per-particle write promotes it out of the constant block
	}
	Data[A][Particle * SourceParticleAttr::Components(Attr) + Component] = Value;
}

FVector3f FSourceParticleCollection::GetVector(ESourceParticleAttr Attr, int32 Particle) const
{
	return FVector3f(GetFloat(Attr, Particle, 0), GetFloat(Attr, Particle, 1), GetFloat(Attr, Particle, 2));
}

void FSourceParticleCollection::SetVector(ESourceParticleAttr Attr, int32 Particle, const FVector3f& Value)
{
	SetFloat(Attr, Particle, Value.X, 0);
	SetFloat(Attr, Particle, Value.Y, 1);
	SetFloat(Attr, Particle, Value.Z, 2);
}

void FSourceParticleCollection::SnapshotInitial(int32 Start, int32 RangeCount, TConstArrayView<ESourceParticleAttr> Attrs)
{
	for (ESourceParticleAttr Attr : Attrs)
	{
		const int32 A = (int32)Attr;
		const int32 Comps = SourceParticleAttr::Components(Attr);
		if (Initial[A].Num() == 0)
		{
			Initial[A].SetNumZeroed(MaxCount * Comps);
		}
		for (int32 P = Start; P < Start + RangeCount; ++P)
		{
			for (int32 C = 0; C < Comps; ++C)
			{
				Initial[A][P * Comps + C] = GetFloat(Attr, P, C);
			}
		}
	}
}

float FSourceParticleCollection::GetInitialFloat(ESourceParticleAttr Attr, int32 Particle, int32 Component) const
{
	const int32 A = (int32)Attr;
	if (Initial[A].Num() == 0)
	{
		return GetFloat(Attr, Particle, Component);
	}
	return Initial[A][Particle * SourceParticleAttr::Components(Attr) + Component];
}

FVector3f FSourceParticleCollection::GetInitialVector(ESourceParticleAttr Attr, int32 Particle) const
{
	return FVector3f(GetInitialFloat(Attr, Particle, 0), GetInitialFloat(Attr, Particle, 1), GetInitialFloat(Attr, Particle, 2));
}

void FSourceParticleCollection::KillAll()
{
	for (int32 P = 0; P < Count; ++P)
	{
		Killed[P] = 1;
	}
}

int32 FSourceParticleCollection::ApplyKills()
{
	int32 Write = 0;
	for (int32 Read = 0; Read < Count; ++Read)
	{
		if (Killed[Read])
		{
			continue;
		}
		if (Write != Read)
		{
			Ids[Write] = Ids[Read];
			for (int32 A = 0; A < SourceParticleAttr::Num; ++A)
			{
				const int32 Comps = SourceParticleAttr::Components((ESourceParticleAttr)A);
				if (Data[A].Num() > 0)
				{
					FMemory::Memcpy(&Data[A][Write * Comps], &Data[A][Read * Comps], Comps * sizeof(float));
				}
				if (Initial[A].Num() > 0)
				{
					FMemory::Memcpy(&Initial[A][Write * Comps], &Initial[A][Read * Comps], Comps * sizeof(float));
				}
			}
		}
		Killed[Write] = 0;
		++Write;
	}
	const int32 Removed = Count - Write;
	Count = Write;
	return Removed;
}

// ---- Control points ---------------------------------------------------------------------------------------------

void FSourceParticleControlPoint::SetOrientationFromAngles(const FVector3f& Angles)
{
	// mathlib AngleVectors(angles, forward, right, up), angles as (pitch, yaw, roll) in degrees.
	float SP, CP, SY, CY, SR, CR;
	FMath::SinCos(&SP, &CP, FMath::DegreesToRadians(Angles.X));
	FMath::SinCos(&SY, &CY, FMath::DegreesToRadians(Angles.Y));
	FMath::SinCos(&SR, &CR, FMath::DegreesToRadians(Angles.Z));
	Forward = FVector3f(CP * CY, CP * SY, -SP);
	Right = FVector3f(-1 * SR * SP * CY + -1 * CR * -SY, -1 * SR * SP * SY + -1 * CR * CY, -1 * SR * CP);
	Up = FVector3f(CR * SP * CY + -SR * -SY, CR * SP * SY + -SR * CY, CR * CP);
}

float FSourceParticleContext::CPField(int32 Index, int32 Field) const
{
	const FVector3f& P = CP(Index).Position;
	return Field == 1 ? P.Y : (Field == 2 ? P.Z : P.X);
}

// ---- Random -----------------------------------------------------------------------------------------------------

float FSourceParticleRandom::At(int32 Index)
{
	static const TArray<float> Table = []()
	{
		// splitmix64 fill, so every machine draws the same sequence.
		TArray<float> T;
		T.SetNumUninitialized(Size);
		uint64 State = 0x9E3779B97F4A7C15ull;
		for (int32 i = 0; i < Size; ++i)
		{
			State += 0x9E3779B97F4A7C15ull;
			uint64 Z = State;
			Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull;
			Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull;
			Z ^= Z >> 31;
			T[i] = (float)(Z >> 40) * (1.0f / 16777216.0f);
		}
		return T;
	}();
	return Table[Index & Mask];
}

float FSourceParticleRandom::RangeExp(int32 Seed, int32 Sample, float Min, float Max, float Exponent)
{
	float T = Float(Seed, Sample);
	if (Exponent != 1.0f)
	{
		T = FMath::Pow(T, Exponent);
	}
	return Min + T * (Max - Min);
}

// ---- Math -------------------------------------------------------------------------------------------------------

namespace SourceParticleMath
{
	float RemapClamped(float Value, float InMin, float InMax)
	{
		if (InMax == InMin)
		{
			return Value >= InMax ? 1.0f : 0.0f;
		}
		return Clamp01((Value - InMin) / (InMax - InMin));
	}

	float Bias(float T, float B)
	{
		if (B <= 0.0f)
		{
			return 0.0f;
		}
		return T / ((1.0f / B - 2.0f) * (1.0f - T) + 1.0f);
	}

	float FadeCurve(float T, bool bEaseInAndOut, float BiasValue)
	{
		T = Clamp01(T);
		if (BiasValue != 0.5f && BiasValue > 0.0f)
		{
			T = Bias(T, BiasValue);
		}
		return bEaseInAndOut ? SimpleSpline(T) : T;
	}

	float DragAdjusted(float Drag, float Dt)
	{
		const float Keep = 1.0f - Drag;
		if (Keep <= 0.0f)
		{
			return 0.0f;
		}
		if (Keep >= 1.0f)
		{
			return 1.0f;
		}
		return FMath::Pow(Keep, Dt * 30.0f);
	}

	float Noise1D(float Coordinate)
	{
		const int32 Cell = FMath::FloorToInt(Coordinate);
		const float Frac = Coordinate - Cell;
		const float A = FSourceParticleRandom::At(Cell * 89) * 2.0f - 1.0f;
		const float B = FSourceParticleRandom::At((Cell + 1) * 89) * 2.0f - 1.0f;
		return Lerp(A, B, SimpleSpline(Frac));
	}

	FVector3f Noise3D(const FVector3f& Coordinate)
	{
		const float C = Coordinate.X + Coordinate.Y * 57.0f + Coordinate.Z * 131.0f;
		return FVector3f(Noise1D(C), Noise1D(C + 1024.5f), Noise1D(C + 2048.25f));
	}

	FVector3f RandomUnitVector(int32 Seed, int32 Sample)
	{
		const FVector3f V(FSourceParticleRandom::Float(Seed, Sample) * 2.0f - 1.0f,
			FSourceParticleRandom::Float(Seed + 1, Sample) * 2.0f - 1.0f,
			FSourceParticleRandom::Float(Seed + 2, Sample) * 2.0f - 1.0f);
		const float LengthSq = V.SizeSquared();
		if (LengthSq < 1e-8f)
		{
			return FVector3f(0, 0, 1);
		}
		return V / FMath::Sqrt(LengthSq);
	}

	void VectorVectors(const FVector3f& Forward, FVector3f& OutRight, FVector3f& OutUp)
	{
		if (FMath::Abs(Forward.X) < 1e-6f && FMath::Abs(Forward.Y) < 1e-6f)
		{
			OutRight = FVector3f(0, -1, 0);
			OutUp = FVector3f(-Forward.Z, 0, 0);
			return;
		}
		OutRight = FVector3f::CrossProduct(Forward, FVector3f(0, 0, 1)).GetSafeNormal();
		OutUp = FVector3f::CrossProduct(OutRight, Forward).GetSafeNormal();
	}

	FVector3f RotateAboutAxis(const FVector3f& V, const FVector3f& UnitAxis, float Radians)
	{
		return FQuat4f(UnitAxis, Radians).RotateVector(V);
	}
}

// ---- FSourceParticleOp ------------------------------------------------------------------------------------------

void FSourceParticleOp::Init(const FSourceDMXElement* InElement, int32 InInstanceSeed)
{
	Element = InElement;
	InstanceSeed = InInstanceSeed;
	OpStartFadeIn = F(TEXT("operator start fadein"), 0.0f);
	OpEndFadeIn = F(TEXT("operator end fadein"), 0.0f);
	OpStartFadeOut = F(TEXT("operator start fadeout"), 0.0f);
	OpEndFadeOut = F(TEXT("operator end fadeout"), 0.0f);
	OpFadeOscillate = F(TEXT("operator fade oscillate"), 0.0f);
	OpTimeOffsetSeed = I(TEXT("operator time offset seed"), 0);
	OpTimeOffsetMin = F(TEXT("operator time offset min"), 0.0f);
	OpTimeOffsetMax = F(TEXT("operator time offset max"), 0.0f);
	OpTimeScaleSeed = I(TEXT("operator time scale seed"), 0);
	OpTimeScaleMin = F(TEXT("operator time scale min"), 1.0f);
	OpTimeScaleMax = F(TEXT("operator time scale max"), 1.0f);
	OpStrengthScaleSeed = I(TEXT("operator strength scale seed"), 0);
	OpStrengthMin = F(TEXT("operator strength random scale min"), 1.0f);
	OpStrengthMax = F(TEXT("operator strength random scale max"), 1.0f);
	OpEndCapState = I(TEXT("operator end cap state"), -1);
	Configure();
}

float FSourceParticleOp::Strength(const FSourceParticleContext& Ctx) const
{
	// End-cap gating: -1 always; 0 only while NOT in the end cap; 1 only during it.
	if (OpEndCapState == 0 && Ctx.bInEndCap) { return 0.0f; }
	if (OpEndCapState == 1 && !Ctx.bInEndCap) { return 0.0f; }

	float T = Ctx.Time;
	if (OpTimeOffsetSeed != 0)
	{
		T += FSourceParticleRandom::Range(OpTimeOffsetSeed + InstanceSeed, 0, OpTimeOffsetMin, OpTimeOffsetMax);
	}
	// Oscillate: the envelope time becomes fmod(t / period, 1) and the fade times read in 0..1.
	if (OpFadeOscillate > 0.0f)
	{
		const float Phase = T / OpFadeOscillate;
		T = Phase - FMath::Floor(Phase);
	}
	if (OpTimeScaleSeed != 0)
	{
		const float Scale = FSourceParticleRandom::Range(OpTimeScaleSeed + InstanceSeed, 1, OpTimeScaleMin, OpTimeScaleMax);
		if (Scale != 0.0f)
		{
			T = OpStartFadeIn + (T - OpStartFadeIn) / Scale;
		}
	}

	float Result = 1.0f;
	if (OpEndFadeIn != OpStartFadeIn)
	{
		if (T < OpStartFadeIn) { Result = 0.0f; }
		else if (T < OpEndFadeIn) { Result = (T - OpStartFadeIn) / (OpEndFadeIn - OpStartFadeIn); }
	}
	if (OpEndFadeOut != OpStartFadeOut)
	{
		if (T > OpEndFadeOut) { Result = 0.0f; }
		else if (T > OpStartFadeOut) { Result *= 1.0f - (T - OpStartFadeOut) / (OpEndFadeOut - OpStartFadeOut); }
	}
	if (OpStrengthScaleSeed != 0)
	{
		Result *= FSourceParticleRandom::Range(OpStrengthScaleSeed + InstanceSeed, 2, OpStrengthMin, OpStrengthMax);
	}
	return FMath::Clamp(Result, 0.0f, 1.0f);
}

// ---- FSourceParticleChildSim -------------------------------------------------------------------------------------

FSourceParticleChildSim::FSourceParticleChildSim() = default;
FSourceParticleChildSim::~FSourceParticleChildSim() = default;
FSourceParticleChildSim::FSourceParticleChildSim(FSourceParticleChildSim&&) = default;
FSourceParticleChildSim& FSourceParticleChildSim::operator=(FSourceParticleChildSim&&) = default;

// ---- FSourceParticleSimulator ------------------------------------------------------------------------------------

FSourceParticleSimulator::FSourceParticleSimulator(TSharedPtr<const FSourceParticleDefinition> InDefinition)
	: Definition(InDefinition)
{
	TSet<const FSourceParticleDefinition*> Ancestors;
	Construct(Ancestors);
}

FSourceParticleSimulator::FSourceParticleSimulator(TSharedPtr<const FSourceParticleDefinition> InDefinition,
	TSet<const FSourceParticleDefinition*>& Ancestors)
	: Definition(InDefinition)
{
	Construct(Ancestors);
}

FSourceParticleSimulator::~FSourceParticleSimulator() = default;

void FSourceParticleSimulator::Construct(TSet<const FSourceParticleDefinition*>& Ancestors)
{
	check(Definition.IsValid());
	// Category bases keep each instance's seed unique and stable from run to run.
	BuildOps(Definition->Emitters, TEXT("emitters"), Emitters, 1000);
	BuildOps(Definition->Initializers, TEXT("initializers"), Initializers, 2000);
	BuildOps(Definition->Operators, TEXT("operators"), Operators, 3000);
	BuildOps(Definition->Forces, TEXT("forces"), Forces, 4000);
	BuildOps(Definition->Constraints, TEXT("constraints"), Constraints, 5000);
	BuildOps(Definition->Renderers, TEXT("renderers"), Renderers, 6000);
	Context.Forces = &Forces;
	Context.Constraints = &Constraints;

	Ancestors.Add(Definition.Get());
	for (const FSourceParticleChildDef& Link : Definition->Children)
	{
		if (!Link.Definition.IsValid() || Ancestors.Contains(Link.Definition.Get()))
		{
			continue;	// unresolved, or a cycle
		}
		FSourceParticleChildSim Child;
		Child.Link = Link;
		Child.Simulator = TUniquePtr<FSourceParticleSimulator>(new FSourceParticleSimulator(Link.Definition, Ancestors));
		Children.Add(MoveTemp(Child));
	}
	Ancestors.Remove(Definition.Get());
	Context.Children = &Children;

	Reset();
}

void FSourceParticleSimulator::BuildOps(const TArray<FSourceParticleOperatorDef>& Defs, const TCHAR* Category,
	TArray<TUniquePtr<FSourceParticleOp>>& Out, int32 SeedBase)
{
	Out.Reset();
	for (int32 i = 0; i < Defs.Num(); ++i)
	{
		Out.Add(SourceParticleOps::Create(Category, Defs[i].FunctionName, Defs[i].Element, SeedBase + i * 17));
	}
}

void FSourceParticleSimulator::Reset()
{
	Particles = MakeUnique<FSourceParticleCollection>(Definition->MaxParticles);
	SeedConstants();

	Context.Time = 0.0f;
	Context.DeltaTime = 0.0f;
	Context.PreviousDeltaTime = 0.0f;
	Context.bInEndCap = false;
	Context.bEmissionStopped = false;
	Context.OperatorRandomOffset = 0;

	for (TArray<TUniquePtr<FSourceParticleOp>>* List : { &Emitters, &Initializers, &Operators, &Forces, &Constraints, &Renderers })
	{
		for (TUniquePtr<FSourceParticleOp>& Op : *List)
		{
			Op->Reset();
		}
	}

	bFirstFrameDone = false;
	for (FSourceParticleChildSim& Child : Children)
	{
		Child.bStarted = false;
		Child.OverriddenControlPoints.Reset();
		Child.Simulator->Reset();
	}
}

void FSourceParticleSimulator::SeedConstants()
{
	// The definition's constant block: what unwritten attributes read as.
	Particles->SetConstant(ESourceParticleAttr::Radius, 0, Definition->Radius);
	Particles->SetConstant(ESourceParticleAttr::TintRgb, 0, Definition->Color.R / 255.0f);
	Particles->SetConstant(ESourceParticleAttr::TintRgb, 1, Definition->Color.G / 255.0f);
	Particles->SetConstant(ESourceParticleAttr::TintRgb, 2, Definition->Color.B / 255.0f);
	Particles->SetConstant(ESourceParticleAttr::Alpha, 0, Definition->Color.A / 255.0f);
	Particles->SetConstant(ESourceParticleAttr::Rotation, 0, Definition->Rotation * SourceParticleMath::DegToRad);
	Particles->SetConstant(ESourceParticleAttr::RotationSpeed, 0, Definition->RotationSpeed * SourceParticleMath::DegToRad);
	Particles->SetConstant(ESourceParticleAttr::Normal, 0, Definition->Normal.X);
	Particles->SetConstant(ESourceParticleAttr::Normal, 1, Definition->Normal.Y);
	Particles->SetConstant(ESourceParticleAttr::Normal, 2, Definition->Normal.Z);
	Particles->SetConstant(ESourceParticleAttr::SequenceNumber, 0, (float)Definition->SequenceNumber);
	Particles->SetConstant(ESourceParticleAttr::SequenceNumber1, 0, (float)Definition->SequenceNumber1);
}

void FSourceParticleSimulator::Update(float DeltaTime)
{
	if (!bFirstFrameDone)
	{
		SimulateFirstFrame();
		bFirstFrameDone = true;
	}
	if (DeltaTime > 0.0f && Context.Time < Definition->FreezeSimulationAfterTime)
	{
		const float MaxStep = Definition->MaximumTimeStep > 0.0f ? Definition->MaximumTimeStep : 0.1f;
		const int32 SubSteps = FMath::Clamp(FMath::CeilToInt(DeltaTime / MaxStep), 1, 10);
		const float StepDt = DeltaTime / SubSteps;
		for (int32 Step = 0; Step < SubSteps; ++Step)
		{
			Substep(StepDt);
		}
	}

	// Children first: they copy this system's control points and want the frame's position/previous pair
	// intact for their velocity reads.
	for (FSourceParticleChildSim& Child : Children)
	{
		UpdateChild(Child, DeltaTime);
	}

	// The velocity baseline for the next frame; the host writes new positions in between.
	for (FSourceParticleControlPoint& CP : Context.ControlPoints)
	{
		CP.PreviousPosition = CP.Position;
	}
}

void FSourceParticleSimulator::SimulateFirstFrame()
{
	Context.Time = 0.0f;
	Context.DeltaTime = 0.0f;
	RunPreEmitterOperators();
	if (Definition->InitialParticles > 0)
	{
		int32 Spawned = 0;
		const int32 Start = Particles->Spawn(Definition->InitialParticles, Spawned);
		for (int32 i = 0; i < Spawned; ++i)
		{
			Particles->SetFloat(ESourceParticleAttr::CreationTime, Start + i, 0.0f);
		}
		InitializeNewParticles(Start, Spawned);
	}
}

void FSourceParticleSimulator::Substep(float StepDt)
{
	Context.Time += StepDt;
	// The step before this one, which the verlet deltas were made over; the first step counts as its own.
	Context.PreviousDeltaTime = Context.DeltaTime > 0.0f ? Context.DeltaTime : StepDt;
	Context.DeltaTime = StepDt;

	RunPreEmitterOperators();

	const int32 FirstNew = Particles->Num();
	for (TUniquePtr<FSourceParticleOp>& Emitter : Emitters)
	{
		// Emission can be halted by Stop Effect after Duration; the offset still advances so the later
		// operators' random draws stay where they were.
		if (!Context.bEmissionStopped)
		{
			const float S = Emitter->Strength(Context);
			if (S > 0.0f)
			{
				Emitter->Emit(*Particles, Context, S);
			}
		}
		Context.AdvanceOperatorOffset();
	}
	if (Particles->Num() > FirstNew)
	{
		InitializeNewParticles(FirstNew, Particles->Num() - FirstNew);
	}

	for (TUniquePtr<FSourceParticleOp>& Op : Operators)
	{
		if (Op->RunBeforeEmitters())
		{
			Context.AdvanceOperatorOffset();
			continue;
		}
		const float S = Op->Strength(Context);
		if (S > 0.0f)
		{
			Op->Operate(*Particles, Context, S);
		}
		Context.AdvanceOperatorOffset();
		Particles->ApplyKills();
	}
}

void FSourceParticleSimulator::RunPreEmitterOperators()
{
	for (TUniquePtr<FSourceParticleOp>& Op : Operators)
	{
		if (!Op->RunBeforeEmitters())
		{
			continue;
		}
		const float S = Op->Strength(Context);
		if (S > 0.0f)
		{
			Op->Operate(*Particles, Context, S);
		}
		Particles->ApplyKills();
	}
}

void FSourceParticleSimulator::InitializeNewParticles(int32 Start, int32 Count)
{
	for (TUniquePtr<FSourceParticleOp>& Initializer : Initializers)
	{
		const float S = Initializer->Strength(Context);
		if (S > 0.0f)
		{
			Initializer->InitNewParticles(*Particles, Context, Start, Count, S);
		}
		Context.AdvanceOperatorOffset();
	}
	// The attributes the "initial value" reads want, snapshotted after initialization.
	static const ESourceParticleAttr Snapshot[] =
	{
		ESourceParticleAttr::Radius, ESourceParticleAttr::Alpha, ESourceParticleAttr::Alpha2,
		ESourceParticleAttr::TintRgb, ESourceParticleAttr::GlowRgb, ESourceParticleAttr::Xyz,
	};
	Particles->SnapshotInitial(Start, Count, Snapshot);
	Particles->ApplyKills();
}

void FSourceParticleSimulator::UpdateChild(FSourceParticleChildSim& Child, float DeltaTime)
{
	// Children read the parent's control points and particles. Points a child-CP operator set this frame are
	// kept, then the override set is cleared for the next.
	FSourceParticleContext& ChildCtx = Child.Simulator->Context;
	for (int32 i = 0; i < FSourceParticleContext::MaxControlPoints; ++i)
	{
		if (!Child.OverriddenControlPoints.Contains(i))
		{
			ChildCtx.ControlPoints[i] = Context.ControlPoints[i];
		}
	}
	Child.OverriddenControlPoints.Reset();
	ChildCtx.ParentParticles = Particles.Get();

	if (!Child.bStarted)
	{
		if (Context.Time < Child.Link.Delay)
		{
			return;
		}
		Child.bStarted = true;
	}
	Child.Simulator->Update(DeltaTime);
}

void FSourceParticleSimulator::StopEmission(bool bPlayEndCap)
{
	Context.bEmissionStopped = true;
	if (bPlayEndCap)
	{
		Context.bInEndCap = true;
	}
	for (FSourceParticleChildSim& Child : Children)
	{
		Child.Simulator->StopEmission(bPlayEndCap);
	}
}

void FSourceParticleSimulator::KillAllParticles()
{
	Particles->KillAll();
	Particles->ApplyKills();
	for (FSourceParticleChildSim& Child : Children)
	{
		Child.Simulator->KillAllParticles();
	}
}

void FSourceParticleSimulator::SetWorldHooks(const FSourceParticleContext& Template)
{
	ForEachSystem([&Template](FSourceParticleSimulator& System)
	{
		System.Context.TraceLine = Template.TraceLine;
		System.Context.bHavePlayer = Template.bHavePlayer;
		System.Context.PlayerPosition = Template.PlayerPosition;
		System.Context.PlayerEyeAngles = Template.PlayerEyeAngles;
	});
}

void FSourceParticleSimulator::SetControlPoint(int32 Index, const FVector3f& Position, bool bResetPrevious)
{
	if (Index < 0 || Index >= FSourceParticleContext::MaxControlPoints)
	{
		return;
	}
	ForEachSystem([Index, &Position, bResetPrevious](FSourceParticleSimulator& System)
	{
		FSourceParticleControlPoint& CP = System.Context.ControlPoints[Index];
		CP.Position = Position;
		if (bResetPrevious)
		{
			CP.PreviousPosition = Position;
		}
	});
}

void FSourceParticleSimulator::SetControlPointOrientation(int32 Index, const FVector3f& Forward, const FVector3f& Right, const FVector3f& Up)
{
	if (Index < 0 || Index >= FSourceParticleContext::MaxControlPoints)
	{
		return;
	}
	ForEachSystem([Index, &Forward, &Right, &Up](FSourceParticleSimulator& System)
	{
		FSourceParticleControlPoint& CP = System.Context.ControlPoints[Index];
		CP.Forward = Forward;
		CP.Right = Right;
		CP.Up = Up;
	});
}

void FSourceParticleSimulator::ForEachSystem(TFunctionRef<void(FSourceParticleSimulator&)> Visit)
{
	Visit(*this);
	for (FSourceParticleChildSim& Child : Children)
	{
		Child.Simulator->ForEachSystem(Visit);
	}
}

float FSourceParticleSimulator::ComputeEmittersEndTime(float Delay) const
{
	float End = 0.0f;
	for (int32 i = 0; i < Definition->Emitters.Num(); ++i)
	{
		const FSourceParticleOperatorDef& Def = Definition->Emitters[i];
		const FSourceDMXElement* E = Def.Element;
		if (!E)
		{
			continue;
		}
		if (Def.FunctionName.Equals(TEXT("emit_continuously"), ESearchCase::IgnoreCase))
		{
			const float Duration = E->GetFloat(TEXT("emission_duration"), 0.0f);
			End = FMath::Max(End, Duration <= 0.0f ? FLT_MAX : E->GetFloat(TEXT("emission_start_time"), 0.0f) + Duration);
		}
		else if (Def.FunctionName.Equals(TEXT("emit noise"), ESearchCase::IgnoreCase))
		{
			const float Duration = E->GetFloat(TEXT("emission_duration"), 0.0f);
			End = FMath::Max(End, Duration <= 0.0f ? FLT_MAX : E->GetFloat(TEXT("emission_start_time"), 0.0f) + Duration);
		}
		else if (Def.FunctionName.Equals(TEXT("emit_instantaneously"), ESearchCase::IgnoreCase))
		{
			End = FMath::Max(End, FMath::Max(E->GetFloat(TEXT("emission_start_time"), 0.0f), E->GetFloat(TEXT("emission_start_time max"), -1.0f)) + 0.1f);
		}
		else
		{
			End = FLT_MAX;	// maintain-count and anything unknown: endless
		}
		// The envelope can switch an endless emitter off for good.
		if (End == FLT_MAX && Emitters.IsValidIndex(i))
		{
			const float FadeOutEnd = E->GetFloat(TEXT("operator end fadeout"), 0.0f);
			const float FadeOutStart = E->GetFloat(TEXT("operator start fadeout"), 0.0f);
			if (FadeOutEnd > FadeOutStart && E->GetFloat(TEXT("operator fade oscillate"), 0.0f) <= 0.0f)
			{
				End = FadeOutEnd;
			}
		}
	}
	for (const FSourceParticleChildSim& Child : Children)
	{
		const float ChildEnd = Child.Simulator->ComputeEmittersEndTime(Delay + Child.Link.Delay);
		End = FMath::Max(End, ChildEnd);
	}
	return End == FLT_MAX ? FLT_MAX : End + Delay;
}

int32 FSourceParticleSimulator::TotalParticleCount() const
{
	int32 Total = Particles->Num();
	for (const FSourceParticleChildSim& Child : Children)
	{
		Total += Child.Simulator->TotalParticleCount();
	}
	return Total;
}
