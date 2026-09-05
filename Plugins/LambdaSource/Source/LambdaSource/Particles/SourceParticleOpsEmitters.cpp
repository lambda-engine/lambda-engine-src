#include "Particles/SourceParticleOpsInternal.h"

// The emitters (particles/builtin_particle_emitters.cpp). Each spawns a number of particles for the step and
// spreads their creation times across the step's window, so a steady stream does not clump on frame edges.

using namespace SourceParticleOpHelpers;

namespace
{
	class FEmitterOp : public FSourceParticleOp
	{
	protected:
		static void SpawnSpread(FSourceParticleCollection& P, float WindowStart, float WindowEnd, int32 Requested)
		{
			int32 Spawned = 0;
			const int32 Start = P.Spawn(Requested, Spawned);
			for (int32 i = 0; i < Spawned; ++i)
			{
				const float T = Spawned > 1 ? (i + 1) / (float)Spawned : 1.0f;
				P.SetFloat(ESourceParticleAttr::CreationTime, Start + i, Lerp(WindowStart, WindowEnd, T));
			}
		}

		/** The "emission count scale control point" option: a rate multiplier read off a control point. */
		static float ControlPointScale(const FSourceParticleContext& Ctx, int32 CP, int32 Field)
		{
			if (CP < 0 || CP >= FSourceParticleContext::MaxControlPoints)
			{
				return 1.0f;
			}
			return Ctx.CPField(CP, Field);
		}
	};

	/** emit_continuously: a fixed rate times the strength, with the fraction carried over between frames. */
	class FEmitContinuously : public FEmitterOp
	{
		float StartTime = 0.0f, Rate = 100.0f, Duration = 0.0f;
		int32 ScaleCP = -1, ScaleField = 0;
		bool bForKilledParents = false;
		float Remainder = 0.0f;

		virtual void Configure() override
		{
			StartTime = F(TEXT("emission_start_time"), 0.0f);
			Rate = F(TEXT("emission_rate"), 100.0f);
			Duration = F(TEXT("emission_duration"), 0.0f);
			ScaleCP = I(TEXT("emission count scale control point"), -1);
			ScaleField = I(TEXT("emission count scale control point field"), 0);
			bForKilledParents = B(TEXT("emit particles for killed parent particles"), false);
		}
		virtual void Reset() override { Remainder = 0.0f; }

		virtual void Emit(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (bForKilledParents)
			{
				return;	// emission in answer to parent deaths is not simulated
			}
			const float WindowStart = FMath::Max(Ctx.Time - Ctx.DeltaTime, StartTime);
			float WindowEnd = Ctx.Time;
			if (Duration > 0.0f)
			{
				WindowEnd = FMath::Min(WindowEnd, StartTime + Duration);
			}
			if (WindowEnd <= WindowStart)
			{
				return;
			}
			const float ActualRate = Rate * Strength * ControlPointScale(Ctx, ScaleCP, ScaleField);
			if (ActualRate <= 0.0f)
			{
				return;
			}
			const float Exact = ActualRate * (WindowEnd - WindowStart) + Remainder;
			const int32 Count = (int32)Exact;
			Remainder = Exact - Count;
			if (Count > 0)
			{
				SpawnSpread(P, WindowStart, WindowEnd, Count);
			}
		}
	};

	/** emit_instantaneously: one burst, optionally spread over several frames. */
	class FEmitInstantaneously : public FEmitterOp
	{
		float StartTime = 0.0f, StartTimeMax = -1.0f;
		int32 NumToEmit = 100, NumToEmitMinimum = -1, MaxPerFrame = -1, ScaleCP = -1, ScaleField = 0;
		int32 Remaining = -1;

		virtual void Configure() override
		{
			StartTime = F(TEXT("emission_start_time"), 0.0f);
			StartTimeMax = F(TEXT("emission_start_time max"), -1.0f);
			NumToEmit = I(TEXT("num_to_emit"), 100);
			NumToEmitMinimum = I(TEXT("num_to_emit_minimum"), -1);
			MaxPerFrame = I(TEXT("maximum emission per frame"), -1);
			ScaleCP = I(TEXT("emission count scale control point"), -1);
			ScaleField = I(TEXT("emission count scale control point field"), 0);
		}
		virtual void Reset() override { Remaining = -1; }

		virtual void Emit(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			float Start = StartTime;
			if (StartTimeMax > Start)
			{
				Start = FSourceParticleRandom::Range(InstanceSeed, 3, Start, StartTimeMax);
			}
			if (Ctx.Time < Start)
			{
				return;
			}
			if (Remaining < 0)
			{
				int32 Count = NumToEmit;
				if (NumToEmitMinimum >= 0)
				{
					const int32 Lo = FMath::Min(NumToEmitMinimum, NumToEmit);
					const int32 Hi = FMath::Max(NumToEmitMinimum, NumToEmit);
					Count = (int32)FSourceParticleRandom::Range(InstanceSeed, 4, (float)Lo, Hi + 0.999f);
				}
				const float Scale = ControlPointScale(Ctx, ScaleCP, ScaleField);
				if (Scale != 1.0f)
				{
					Count = (int32)(Count * Scale);
				}
				Remaining = FMath::Max(0, Count);
			}
			if (Remaining == 0)
			{
				return;
			}
			const int32 ToEmit = MaxPerFrame > 0 ? FMath::Min(Remaining, MaxPerFrame) : Remaining;
			Remaining -= ToEmit;
			SpawnSpread(P, Ctx.Time, Ctx.Time, ToEmit);
		}
	};

	/** emit noise: a continuous emitter whose rate wanders with a noise field. */
	class FEmitNoise : public FEmitterOp
	{
		float StartTime = 0.0f, Duration = 0.0f, TimeScale = 0.1f, TimeOffset = 0.0f, WorldTimeScale = 0.0f;
		float EmissionMin = 0.0f, EmissionMax = 100.0f;
		bool bAbs = false, bInvertAbs = false;
		float Remainder = 0.0f;

		virtual void Configure() override
		{
			StartTime = F(TEXT("emission_start_time"), 0.0f);
			Duration = F(TEXT("emission_duration"), 0.0f);
			TimeScale = F(TEXT("time noise coordinate scale"), 0.1f);
			TimeOffset = F(TEXT("time coordinate offset"), 0.0f);
			WorldTimeScale = F(TEXT("world time noise coordinate scale"), 0.0f);
			EmissionMin = F(TEXT("emission minimum"), 0.0f);
			EmissionMax = F(TEXT("emission maximum"), 100.0f);
			bAbs = B(TEXT("absolute value"), false);
			bInvertAbs = B(TEXT("invert absolute value"), false);
		}
		virtual void Reset() override { Remainder = 0.0f; }

		virtual void Emit(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const float WindowStart = FMath::Max(Ctx.Time - Ctx.DeltaTime, StartTime);
			float WindowEnd = Ctx.Time;
			if (Duration > 0.0f)
			{
				WindowEnd = FMath::Min(WindowEnd, StartTime + Duration);
			}
			if (WindowEnd <= WindowStart)
			{
				return;
			}
			// System time stands in for wall-clock time, so a replay is deterministic.
			const float Coordinate = (Ctx.Time + TimeOffset) * TimeScale + Ctx.Time * WorldTimeScale;
			float Noise = Noise1D(Coordinate + InstanceSeed);
			if (bAbs)
			{
				Noise = FMath::Abs(Noise);
				if (bInvertAbs)
				{
					Noise = 1.0f - Noise;
				}
			}
			else
			{
				Noise = Noise * 0.5f + 0.5f;
			}
			const float Rate = Lerp(EmissionMin, EmissionMax, Clamp01(Noise)) * Strength;
			if (Rate <= 0.0f)
			{
				return;
			}
			const float Exact = Rate * (WindowEnd - WindowStart) + Remainder;
			const int32 Count = (int32)Exact;
			Remainder = Exact - Count;
			if (Count > 0)
			{
				SpawnSpread(P, WindowStart, WindowEnd, Count);
			}
		}
	};

	/** emit to maintain count: tops the system up to a target every step. */
	class FEmitToMaintainCount : public FEmitterOp
	{
		float StartTime = 0.0f;
		int32 CountToMaintain = 100, ScaleCP = -1, ScaleField = 0;

		virtual void Configure() override
		{
			StartTime = F(TEXT("emission start time"), 0.0f);
			CountToMaintain = I(TEXT("count to maintain"), 100);
			ScaleCP = I(TEXT("maintain count scale control point"), -1);
			ScaleField = I(TEXT("maintain count scale control point field"), 0);
		}

		virtual void Emit(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (Ctx.Time < StartTime)
			{
				return;
			}
			const float Target = CountToMaintain * ControlPointScale(Ctx, ScaleCP, ScaleField);
			const int32 Deficit = (int32)Target - P.Num();
			if (Deficit > 0)
			{
				SpawnSpread(P, Ctx.Time - Ctx.DeltaTime, Ctx.Time, Deficit);
			}
		}
	};
}

void RegisterSourceParticleEmitterOps(FSourceParticleOpRegistry& R)
{
	LAMBDA_PARTICLE_OP(R, "emitters", "emit_continuously", FEmitContinuously);
	LAMBDA_PARTICLE_OP(R, "emitters", "emit_instantaneously", FEmitInstantaneously);
	LAMBDA_PARTICLE_OP(R, "emitters", "emit noise", FEmitNoise);
	LAMBDA_PARTICLE_OP(R, "emitters", "emit to maintain count", FEmitToMaintainCount);
}
