#include "Particles/SourceParticleOpsInternal.h"

// The per-step operators (particles/builtin_particle_ops.cpp), run over every live particle in file order.
// Stable per-particle draws (fade windows, cull thresholds) index by particle id alone so a particle gets the
// same value every frame; per-frame draws add the advancing operator offset.

using namespace SourceParticleOpHelpers;
using EAttr = ESourceParticleAttr;

namespace
{
	// ---- Movement -----------------------------------------------------------------------------------------------

	/**
	 * Movement Basic: THE movement operator. Verlet integration under gravity with a 30 fps-normalised drag,
	 * then the system's forces and its constraints, relaxed for up to "max constraint passes". Without it
	 * nothing moves and no force or constraint ever runs.
	 */
	class FMovementBasic : public FSourceParticleOp
	{
		FVector3f Gravity = FVector3f::ZeroVector;
		float Drag = 0.0f;
		int32 MaxConstraintPasses = 3;
		TArray<FVector3f> Accelerations;

		virtual void Configure() override
		{
			Gravity = V(TEXT("gravity"), FVector3f::ZeroVector);
			Drag = F(TEXT("drag"), 0.0f);
			MaxConstraintPasses = I(TEXT("max constraint passes"), 3);
		}

		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const float Dt = Ctx.DeltaTime;
			if (Dt <= 0.0f)
			{
				return;
			}
			Accelerations.SetNumUninitialized(P.Num(), EAllowShrinking::No);
			for (int32 i = 0; i < P.Num(); ++i)
			{
				Accelerations[i] = Gravity;
			}
			if (Ctx.Forces)
			{
				for (const TUniquePtr<FSourceParticleOp>& Force : *Ctx.Forces)
				{
					const float S = Force->Strength(Ctx);
					if (S > 0.0f)
					{
						Force->AddForces(P, Ctx, S, Accelerations);
					}
					Ctx.AdvanceOperatorOffset();
				}
			}

			// A verlet delta is a distance per step, so when the step changes size the delta is rescaled by
			// dt / previous dt (C_OP_BasicMovement's adj_dt); without it a hitch frame multiplies every velocity.
			const float TimeScale = Ctx.PreviousDeltaTime > 0.0f ? Dt / Ctx.PreviousDeltaTime : 1.0f;
			const float DragAdjusted = SourceParticleMath::DragAdjusted(Drag, Dt) * TimeScale;
			const float Dt2 = Dt * Dt;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const FVector3f Xyz = P.GetVector(EAttr::Xyz, i);
				const FVector3f VelocityStep = Xyz - P.GetVector(EAttr::PrevXyz, i);
				P.SetVector(EAttr::PrevXyz, i, Xyz);
				P.SetVector(EAttr::Xyz, i, Xyz + VelocityStep * DragAdjusted + Accelerations[i] * Dt2);
			}

			if (!Ctx.Constraints || Ctx.Constraints->Num() == 0)
			{
				return;
			}
			// Relaxation: rounds until a full pass reports no change; the final constraints run once at the end.
			const int32 Passes = FMath::Max(1, MaxConstraintPasses);
			for (int32 Pass = 0; Pass < Passes; ++Pass)
			{
				bool bChanged = false;
				for (const TUniquePtr<FSourceParticleOp>& Constraint : *Ctx.Constraints)
				{
					if (Constraint->IsFinalConstraint())
					{
						continue;
					}
					if (Constraint->Strength(Ctx) > 0.0f)
					{
						bChanged |= Constraint->ApplyConstraint(P, Ctx);
					}
				}
				if (!bChanged)
				{
					break;
				}
			}
			for (const TUniquePtr<FSourceParticleOp>& Constraint : *Ctx.Constraints)
			{
				if (Constraint->IsFinalConstraint() && Constraint->Strength(Ctx) > 0.0f)
				{
					Constraint->ApplyConstraint(P, Ctx);
				}
			}
		}
	};

	/** Movement Lock to Control Point: particles follow the control point's motion, fading out over their life. */
	class FMovementLockToControlPoint : public FSourceParticleOp
	{
		int32 ControlPoint = 0;
		float StartMin = 1.0f, StartMax = 1.0f, StartExp = 1.0f, EndMin = 1.0f, EndMax = 1.0f, EndExp = 1.0f;

		virtual void Configure() override
		{
			ControlPoint = I(TEXT("control_point_number"), 0);
			StartMin = F(TEXT("start_fadeout_min"), 1.0f);
			StartMax = F(TEXT("start_fadeout_max"), 1.0f);
			StartExp = F(TEXT("start_fadeout_exponent"), 1.0f);
			EndMin = F(TEXT("end_fadeout_min"), 1.0f);
			EndMax = F(TEXT("end_fadeout_max"), 1.0f);
			EndExp = F(TEXT("end_fadeout_exponent"), 1.0f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const FSourceParticleControlPoint& CP = Ctx.CP(ControlPoint);
			const FVector3f Delta = CP.Position - CP.PreviousPosition;
			if (Delta.IsZero())
			{
				return;
			}
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float StartFade = StableRandRangeExp(P, i, 0, StartMin, StartMax, StartExp);
				const float EndFade = StableRandRangeExp(P, i, 3, EndMin, EndMax, EndExp);
				const float Weight = 1.0f - RemapClamped(LifeFraction(P, Ctx, i), StartFade, FMath::Max(StartFade, EndFade));
				if (Weight <= 0.0f)
				{
					continue;
				}
				const FVector3f Move = Delta * Weight;
				P.SetVector(EAttr::Xyz, i, P.GetVector(EAttr::Xyz, i) + Move);
				P.SetVector(EAttr::PrevXyz, i, P.GetVector(EAttr::PrevXyz, i) + Move);
			}
		}
	};

	/** Movement Max Velocity. */
	class FMovementMaxVelocity : public FSourceParticleOp
	{
		float MaxVelocity = 0.0f;
		int32 OverrideCP = -1, OverrideField = 0;
		virtual void Configure() override
		{
			MaxVelocity = F(TEXT("Maximum Velocity"), 0.0f);
			OverrideCP = I(TEXT("Override Max Velocity from this CP"), -1);
			OverrideField = I(TEXT("Override CP field"), 0);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const float Dt = Ctx.DeltaTime;
			if (Dt <= 0.0f)
			{
				return;
			}
			float Max = MaxVelocity;
			if (OverrideCP >= 0 && OverrideCP < FSourceParticleContext::MaxControlPoints)
			{
				Max = Ctx.CPField(OverrideCP, OverrideField);
			}
			if (Max <= 0.0f)
			{
				return;
			}
			const float MaxStep = Max * Dt;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const FVector3f Xyz = P.GetVector(EAttr::Xyz, i);
				const FVector3f Step = Xyz - P.GetVector(EAttr::PrevXyz, i);
				const float Length = Step.Size();
				if (Length > MaxStep)
				{
					P.SetVector(EAttr::PrevXyz, i, Xyz - Step * (MaxStep / Length));
				}
			}
		}
	};

	/** Movement Dampen Relative to Control Point: velocity bleeds off near the control point. */
	class FMovementDampenRelativeToControlPoint : public FSourceParticleOp
	{
		int32 ControlPoint = 0;
		float FalloffRange = 100.0f, DampenScale = 1.0f;
		virtual void Configure() override
		{
			ControlPoint = I(TEXT("control_point_number"), 0);
			FalloffRange = F(TEXT("falloff range"), 100.0f);
			DampenScale = F(TEXT("dampen scale"), 1.0f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (FalloffRange <= 0.0f || DampenScale <= 0.0f)
			{
				return;
			}
			const FVector3f CPPos = Ctx.CP(ControlPoint).Position;
			const float Dampen = Clamp01(DampenScale) * Clamp01(Strength);
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const FVector3f Xyz = P.GetVector(EAttr::Xyz, i);
				const float Dist = (Xyz - CPPos).Size();
				if (Dist >= FalloffRange)
				{
					continue;
				}
				const float Keep = Lerp(1.0f - Dampen, 1.0f, Dist / FalloffRange);
				const FVector3f VelStep = Xyz - P.GetVector(EAttr::PrevXyz, i);
				P.SetVector(EAttr::PrevXyz, i, Xyz - VelStep * Keep);
			}
		}
	};

	/** Movement Rotate Particle Around Axis: orbits around an axis through the control point. */
	class FMovementRotateParticleAroundAxis : public FSourceParticleOp
	{
		FVector3f Axis = FVector3f(0, 0, 1);
		float Rate = 180.0f;
		int32 ControlPoint = 0;
		bool bLocal = false;
		virtual void Configure() override
		{
			Axis = V(TEXT("Rotation Axis"), FVector3f(0, 0, 1));
			Rate = F(TEXT("Rotation Rate"), 180.0f);
			ControlPoint = I(TEXT("Control Point"), 0);
			bLocal = B(TEXT("Use Local Space"), false);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const float Dt = Ctx.DeltaTime;
			if (Dt <= 0.0f || Rate == 0.0f)
			{
				return;
			}
			const FSourceParticleControlPoint& CP = Ctx.CP(ControlPoint);
			FVector3f A = bLocal ? CP.TransformLocal(Axis) : Axis;
			if (A.SizeSquared() < 1e-8f)
			{
				return;
			}
			A.Normalize();
			const float Angle = Rate * DegToRad * Dt * Strength;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				P.SetVector(EAttr::Xyz, i, CP.Position + RotateAboutAxis(P.GetVector(EAttr::Xyz, i) - CP.Position, A, Angle));
				P.SetVector(EAttr::PrevXyz, i, CP.Position + RotateAboutAxis(P.GetVector(EAttr::PrevXyz, i) - CP.Position, A, Angle));
			}
		}
	};

	/** Movement Match Particle Velocities: a flocking nudge towards the swarm's mean direction and speed. */
	class FMovementMatchParticleVelocities : public FSourceParticleOp
	{
		float DirectionStrength = 0.25f, SpeedStrength = 0.25f;
		virtual void Configure() override
		{
			DirectionStrength = F(TEXT("Direction Matching Strength"), 0.25f);
			SpeedStrength = F(TEXT("Speed Matching Strength"), 0.25f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (P.Num() == 0)
			{
				return;
			}
			FVector3f Sum = FVector3f::ZeroVector;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				Sum += P.GetVector(EAttr::Xyz, i) - P.GetVector(EAttr::PrevXyz, i);
			}
			const FVector3f Mean = Sum / (float)P.Num();
			const float MeanSpeed = Mean.Size();
			const FVector3f MeanDir = MeanSpeed > 1e-8f ? Mean / MeanSpeed : FVector3f::ZeroVector;
			const float DirS = Clamp01(DirectionStrength) * Clamp01(Strength);
			const float SpeedS = Clamp01(SpeedStrength) * Clamp01(Strength);
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const FVector3f Xyz = P.GetVector(EAttr::Xyz, i);
				const FVector3f Step = Xyz - P.GetVector(EAttr::PrevXyz, i);
				const float Speed = Step.Size();
				const FVector3f Dir = Speed > 1e-8f ? Step / Speed : MeanDir;
				FVector3f NewDir = Dir + (MeanDir - Dir) * DirS;
				if (NewDir.SizeSquared() > 1e-8f)
				{
					NewDir.Normalize();
				}
				const float NewSpeed = Speed + (MeanSpeed - Speed) * SpeedS;
				P.SetVector(EAttr::PrevXyz, i, Xyz - NewDir * NewSpeed);
			}
		}
	};

	/** Movement Maintain Offset: every particle pinned at one offset from a control point. */
	class FMovementMaintainOffset : public FSourceParticleOp
	{
		int32 LocalSpaceCP = -1;
		FVector3f DesiredOffset = FVector3f::ZeroVector;
		bool bScaleByRadius = false;
		virtual void Configure() override
		{
			LocalSpaceCP = I(TEXT("Local Space CP"), -1);
			DesiredOffset = V(TEXT("Desired Offset"), FVector3f::ZeroVector);
			bScaleByRadius = B(TEXT("Scale by Radius"), false);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const FSourceParticleControlPoint& CP = Ctx.CP(LocalSpaceCP >= 0 ? LocalSpaceCP : 0);
			for (int32 i = 0; i < P.Num(); ++i)
			{
				FVector3f Offset = DesiredOffset;
				if (bScaleByRadius)
				{
					Offset *= P.GetFloat(EAttr::Radius, i);
				}
				const FVector3f Target = CP.Position + CP.TransformLocal(Offset);
				const FVector3f Pos = P.GetVector(EAttr::Xyz, i);
				const FVector3f Delta = (Target - Pos) * Strength;
				// PREV_XYZ moves too, so the rigid move adds no verlet velocity.
				P.SetVector(EAttr::Xyz, i, Pos + Delta);
				P.SetVector(EAttr::PrevXyz, i, P.GetVector(EAttr::PrevXyz, i) + Delta);
			}
		}
	};

	/** Movement Maintain Position Along Path: particles pulled towards their slot on the bezier path. */
	class FMovementMaintainPositionAlongPath : public FSourceParticleOp
	{
		float Bulge = 0.0f, MidPoint = 0.5f, Span = 100.0f, Cohesion = 1.0f;
		int32 StartCP = 0, EndCP = 0, BulgeControl = 0;
		bool bLoop = true;
		virtual void Configure() override
		{
			Bulge = F(TEXT("bulge"), 0.0f);
			StartCP = I(TEXT("start control point number"), 0);
			EndCP = I(TEXT("end control point number"), 0);
			BulgeControl = I(TEXT("bulge control 0=random 1=orientation of start pnt 2=orientation of end point"), 0);
			MidPoint = F(TEXT("mid point position"), 0.5f);
			Span = F(TEXT("particles to map from start to end"), 100.0f);
			bLoop = B(TEXT("restart behavior (0 = bounce, 1 = loop )"), true);
			Cohesion = F(TEXT("cohesion strength"), 1.0f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const float Divisor = Span > 0.0f ? Span : 1.0f;
			const float C = Clamp01(Cohesion) * Strength;
			if (C <= 0.0f)
			{
				return;
			}
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float Fr = P.IdOf(i) / Divisor;
				float T;
				if (bLoop)
				{
					T = Fr - FMath::Floor(Fr);
				}
				else
				{
					const float M = Fr - 2.0f * FMath::Floor(Fr / 2.0f);
					T = M <= 1.0f ? M : 2.0f - M;
				}
				const FVector3f Target = Bezier(Ctx, StartCP, EndCP, MidPoint, Bulge, BulgeControl, FVector3f(0, 0, 1), T);
				const FVector3f Pos = P.GetVector(EAttr::Xyz, i);
				const FVector3f Delta = (Target - Pos) * C;
				P.SetVector(EAttr::Xyz, i, Pos + Delta);
				P.SetVector(EAttr::PrevXyz, i, P.GetVector(EAttr::PrevXyz, i) + Delta);
			}
		}
	};

	/** Movement Place On Ground: keeps each particle on whatever is below it (a trace into the world). */
	class FMovementPlaceOnGround : public FSourceParticleOp
	{
		float Offset = 0.0f, MaxTraceLength = 128.0f, TraceOffset = 64.0f;
		bool bKillOnNoCollision = false, bSetNormal = false;
		virtual void Configure() override
		{
			Offset = F(TEXT("offset"), 0.0f);
			bKillOnNoCollision = B(TEXT("kill on no collision"), false);
			bSetNormal = B(TEXT("set normal"), false);
			MaxTraceLength = F(TEXT("max trace length"), 128.0f);
			TraceOffset = F(TEXT("trace offset"), 64.0f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (!Ctx.TraceLine)
			{
				return;
			}
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const FVector3f Pos = P.GetVector(EAttr::Xyz, i);
				const FVector3f From = Pos + FVector3f(0, 0, TraceOffset);
				FSourceParticleTraceHit Hit;
				if (Ctx.TraceLine(From, From - FVector3f(0, 0, MaxTraceLength + TraceOffset), Hit) && Hit.bHit)
				{
					const FVector3f NewPos(Pos.X, Pos.Y, Hit.Position.Z + Offset);
					const FVector3f Delta = NewPos - Pos;
					P.SetVector(EAttr::Xyz, i, NewPos);
					P.SetVector(EAttr::PrevXyz, i, P.GetVector(EAttr::PrevXyz, i) + Delta);
					if (bSetNormal)
					{
						P.SetVector(EAttr::Normal, i, Hit.Normal);
					}
				}
				else if (bKillOnNoCollision)
				{
					P.Kill(i);
				}
			}
		}
	};

	// ---- Lifespan -----------------------------------------------------------------------------------------------

	/** Lifespan Decay: older than LIFE_DURATION is dead. */
	class FLifespanDecay : public FSourceParticleOp
	{
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			for (int32 i = 0; i < P.Num(); ++i)
			{
				if (Age(P, Ctx, i) >= P.GetFloat(EAttr::LifeDuration, i))
				{
					P.Kill(i);
				}
			}
		}
	};

	/** Lifespan Minimum Velocity Decay. */
	class FLifespanMinimumVelocityDecay : public FSourceParticleOp
	{
		float MinimumVelocity = 1.0f;
		virtual void Configure() override { MinimumVelocity = F(TEXT("minimum velocity"), 1.0f); }
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const float Dt = Ctx.DeltaTime;
			if (Dt <= 0.0f)
			{
				return;
			}
			const float MinStep = MinimumVelocity * Dt;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				if ((P.GetVector(EAttr::Xyz, i) - P.GetVector(EAttr::PrevXyz, i)).Size() < MinStep)
				{
					P.Kill(i);
				}
			}
		}
	};

	/** Lifespan Minimum Alpha Decay. */
	class FLifespanMinimumAlphaDecay : public FSourceParticleOp
	{
		float MinimumAlpha = 0.0f;
		virtual void Configure() override { MinimumAlpha = F(TEXT("minimum alpha"), 0.0f); }
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			for (int32 i = 0; i < P.Num(); ++i)
			{
				if (P.GetFloat(EAttr::Alpha, i) * P.GetFloat(EAttr::Alpha2, i) < MinimumAlpha)
				{
					P.Kill(i);
				}
			}
		}
	};

	/** Lifespan Minimum Radius Decay. */
	class FLifespanMinimumRadiusDecay : public FSourceParticleOp
	{
		float MinimumRadius = 1.0f;
		virtual void Configure() override { MinimumRadius = F(TEXT("minimum radius"), 1.0f); }
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			for (int32 i = 0; i < P.Num(); ++i)
			{
				if (P.GetFloat(EAttr::Radius, i) < MinimumRadius)
				{
					P.Kill(i);
				}
			}
		}
	};

	/** Lifespan Maintain Count Decay: the oldest go once there are more than the count, after a delay. */
	class FLifespanMaintainCountDecay : public FSourceParticleOp
	{
		int32 CountToMaintain = 100, ScaleCP = -1, ScaleField = 0;
		float DecayDelay = 0.0f;
		virtual void Configure() override
		{
			CountToMaintain = I(TEXT("count to maintain"), 100);
			DecayDelay = F(TEXT("decay delay"), 0.0f);
			ScaleCP = I(TEXT("maintain count scale control point"), -1);
			ScaleField = I(TEXT("maintain count scale control point field"), 0);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			float Target = (float)CountToMaintain;
			if (ScaleCP >= 0 && ScaleCP < FSourceParticleContext::MaxControlPoints)
			{
				Target *= Ctx.CPField(ScaleCP, ScaleField);
			}
			const int32 Excess = P.Num() - (int32)Target;
			// Particles are in creation order, so the first ones are the oldest.
			for (int32 i = 0; i < Excess && i < P.Num(); ++i)
			{
				if (Age(P, Ctx, i) >= DecayDelay)
				{
					P.Kill(i);
				}
			}
		}
	};

	/** Cull Random: at a random age, a particle is culled with the given chance. */
	class FCullRandom : public FSourceParticleOp
	{
		float StartTime = 0.0f, EndTime = 1.0f, Exp = 1.0f, Percentage = 0.5f;
		virtual void Configure() override
		{
			StartTime = F(TEXT("Cull Start Time"), 0.0f);
			EndTime = F(TEXT("Cull End Time"), 1.0f);
			Exp = F(TEXT("Cull Time Exponent"), 1.0f);
			Percentage = F(TEXT("Cull Percentage"), 0.5f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (Percentage <= 0.0f)
			{
				return;
			}
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float Threshold = StableRandRangeExp(P, i, 0, StartTime, EndTime, Exp);
				const float A = Age(P, Ctx, i);
				if (A >= Threshold && A - Ctx.DeltaTime < Threshold && StableRand(P, i, 3) < Percentage)
				{
					P.Kill(i);
				}
			}
		}
	};

	/** Cull when crossing plane. */
	class FCullWhenCrossingPlane : public FSourceParticleOp
	{
		int32 ControlPoint = 0;
		float PlaneOffset = 0.0f;
		FVector3f Normal = FVector3f(0, 0, 1);
		virtual void Configure() override
		{
			ControlPoint = I(TEXT("Control Point for point on plane"), 0);
			PlaneOffset = F(TEXT("Cull plane offset"), 0.0f);
			Normal = V(TEXT("Plane Normal"), FVector3f(0, 0, 1));
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (Normal.SizeSquared() < 1e-8f)
			{
				return;
			}
			const FVector3f N = Normal.GetSafeNormal();
			const FVector3f Point = Ctx.CP(ControlPoint).Position + N * PlaneOffset;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				if (FVector3f::DotProduct(P.GetVector(EAttr::Xyz, i) - Point, N) < 0.0f)
				{
					P.Kill(i);
				}
			}
		}
	};

	/** Cull when crossing sphere. */
	class FCullWhenCrossingSphere : public FSourceParticleOp
	{
		int32 ControlPoint = 0;
		float CullDistance = 0.0f;
		FVector3f Offset = FVector3f::ZeroVector;
		bool bInside = false;
		virtual void Configure() override
		{
			ControlPoint = I(TEXT("Control Point"), 0);
			CullDistance = F(TEXT("Cull Distance"), 0.0f);
			Offset = V(TEXT("Control Point offset"), FVector3f::ZeroVector);
			bInside = B(TEXT("Cull inside instead of outside"), false);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const FVector3f Center = Ctx.CP(ControlPoint).Position + Offset;
			const float CullSq = CullDistance * CullDistance;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const bool bIsInside = (P.GetVector(EAttr::Xyz, i) - Center).SizeSquared() < CullSq;
				if (bIsInside == bInside)
				{
					P.Kill(i);
				}
			}
		}
	};

	// ---- Alpha --------------------------------------------------------------------------------------------------

	/** Alpha Fade and Decay: in, hold, out over fractions of the life, and death at the end of it. */
	class FAlphaFadeAndDecay : public FSourceParticleOp
	{
		float StartAlpha = 1.0f, EndAlpha = 0.0f, StartFadeIn = 0.0f, EndFadeIn = 0.5f, StartFadeOut = 0.5f, EndFadeOut = 1.0f;
		virtual void Configure() override
		{
			StartAlpha = F(TEXT("start_alpha"), 1.0f);
			EndAlpha = F(TEXT("end_alpha"), 0.0f);
			StartFadeIn = F(TEXT("start_fade_in_time"), 0.0f);
			EndFadeIn = F(TEXT("end_fade_in_time"), 0.5f);
			StartFadeOut = F(TEXT("start_fade_out_time"), 0.5f);
			EndFadeOut = F(TEXT("end_fade_out_time"), 1.0f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float Life = P.GetFloat(EAttr::LifeDuration, i);
				const float A = Age(P, Ctx, i);
				if (A >= Life)
				{
					P.Kill(i);
					continue;
				}
				const float T = Life > 0.0f ? A / Life : 1.0f;
				const float Initial = P.GetInitialFloat(EAttr::Alpha, i);
				float Alpha = Initial;
				if (T < EndFadeIn)
				{
					Alpha = Initial * Lerp(StartAlpha, 1.0f, RemapClamped(T, StartFadeIn, EndFadeIn));
				}
				else if (T > StartFadeOut)
				{
					Alpha = Initial * Lerp(1.0f, EndAlpha, RemapClamped(T, StartFadeOut, EndFadeOut));
				}
				P.SetFloat(EAttr::Alpha, i, Alpha);
			}
		}
	};

	/** Alpha Fade In Random. */
	class FAlphaFadeInRandom : public FSourceParticleOp
	{
		float Min = 0.25f, Max = 0.25f, Exp = 1.0f;
		bool bProportional = true;
		virtual void Configure() override
		{
			Min = F(TEXT("fade in time min"), 0.25f);
			Max = F(TEXT("fade in time max"), 0.25f);
			Exp = F(TEXT("fade in time exponent"), 1.0f);
			bProportional = B(TEXT("proportional 0/1"), true);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			for (int32 i = 0; i < P.Num(); ++i)
			{
				float FadeTime = StableRandRangeExp(P, i, 0, Min, Max, Exp);
				if (bProportional)
				{
					FadeTime *= P.GetFloat(EAttr::LifeDuration, i);
				}
				if (FadeTime <= 0.0f)
				{
					continue;
				}
				const float A = Age(P, Ctx, i);
				if (A < FadeTime)
				{
					P.SetFloat(EAttr::Alpha, i, P.GetInitialFloat(EAttr::Alpha, i) * (A / FadeTime));
				}
			}
		}
	};

	/** Alpha Fade Out Random. */
	class FAlphaFadeOutRandom : public FSourceParticleOp
	{
		float Min = 0.25f, Max = 0.25f, Exp = 1.0f, FadeBias = 0.5f;
		bool bProportional = true, bEase = true;
		virtual void Configure() override
		{
			Min = F(TEXT("fade out time min"), 0.25f);
			Max = F(TEXT("fade out time max"), 0.25f);
			Exp = F(TEXT("fade out time exponent"), 1.0f);
			bProportional = B(TEXT("proportional 0/1"), true);
			bEase = B(TEXT("ease in and out"), true);
			FadeBias = F(TEXT("fade bias"), 0.5f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float Life = P.GetFloat(EAttr::LifeDuration, i);
				float FadeTime = StableRandRangeExp(P, i, 0, Min, Max, Exp);
				if (bProportional)
				{
					FadeTime *= Life;
				}
				if (FadeTime <= 0.0f)
				{
					continue;
				}
				const float FadeStart = Life - FadeTime;
				const float A = Age(P, Ctx, i);
				if (A <= FadeStart)
				{
					continue;
				}
				const float T = FadeCurve((A - FadeStart) / FadeTime, bEase, FadeBias);
				P.SetFloat(EAttr::Alpha, i, P.GetInitialFloat(EAttr::Alpha, i) * (1.0f - T));
			}
		}
	};

	/** Alpha Fade In Simple. */
	class FAlphaFadeInSimple : public FSourceParticleOp
	{
		float FadeIn = 0.25f;
		virtual void Configure() override { FadeIn = F(TEXT("proportional fade in time"), 0.25f); }
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (FadeIn <= 0.0f)
			{
				return;
			}
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float T = LifeFraction(P, Ctx, i);
				if (T < FadeIn)
				{
					P.SetFloat(EAttr::Alpha, i, P.GetInitialFloat(EAttr::Alpha, i) * (T / FadeIn));
				}
			}
		}
	};

	/** Alpha Fade Out Simple. */
	class FAlphaFadeOutSimple : public FSourceParticleOp
	{
		float FadeOut = 0.25f;
		virtual void Configure() override { FadeOut = F(TEXT("proportional fade out time"), 0.25f); }
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (FadeOut <= 0.0f)
			{
				return;
			}
			const float FadeStart = 1.0f - FadeOut;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float T = LifeFraction(P, Ctx, i);
				if (T > FadeStart)
				{
					P.SetFloat(EAttr::Alpha, i, P.GetInitialFloat(EAttr::Alpha, i) * Clamp01((1.0f - T) / FadeOut));
				}
			}
		}
	};

	// ---- Size, colour, rotation ---------------------------------------------------------------------------------

	/** Radius Scale: the radius relative to its initial value, over the life. */
	class FRadiusScale : public FSourceParticleOp
	{
		float StartTime = 0.0f, EndTime = 1.0f, StartScale = 1.0f, EndScale = 1.0f, ScaleBias = 0.5f;
		bool bEase = false;
		virtual void Configure() override
		{
			StartTime = F(TEXT("start_time"), 0.0f);
			EndTime = F(TEXT("end_time"), 1.0f);
			StartScale = F(TEXT("radius_start_scale"), 1.0f);
			EndScale = F(TEXT("radius_end_scale"), 1.0f);
			bEase = B(TEXT("ease_in_and_out"), false);
			ScaleBias = F(TEXT("scale_bias"), 0.5f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float T = FadeCurve(RemapClamped(LifeFraction(P, Ctx, i), StartTime, EndTime), bEase, ScaleBias);
				P.SetFloat(EAttr::Radius, i, P.GetInitialFloat(EAttr::Radius, i) * Lerp(StartScale, EndScale, T));
			}
		}
	};

	/** Color Fade: from the initial colour to the fade colour over a window of the life. */
	class FColorFade : public FSourceParticleOp
	{
		FVector3f Target = FVector3f(1, 1, 1);
		float StartTime = 0.0f, EndTime = 1.0f;
		bool bEase = true;
		EAttr Output = EAttr::TintRgb;
		virtual void Configure() override
		{
			const FColor Color = C(TEXT("color_fade"), FColor::White);
			Target = FVector3f(Color.R, Color.G, Color.B) / 255.0f;
			StartTime = F(TEXT("fade_start_time"), 0.0f);
			EndTime = F(TEXT("fade_end_time"), 1.0f);
			bEase = B(TEXT("ease_in_and_out"), true);
			Output = I(TEXT("output field"), 6) == (int32)EAttr::GlowRgb ? EAttr::GlowRgb : EAttr::TintRgb;
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float T = FadeCurve(RemapClamped(LifeFraction(P, Ctx, i), StartTime, EndTime), bEase, 0.5f);
				P.SetVector(Output, i, FMath::Lerp(P.GetInitialVector(Output, i), Target, T));
			}
		}
	};

	/** Rotation Spin Roll / Yaw: a constant spin rate that winds down to a floor by the stop time. */
	class FRotationSpin : public FSourceParticleOp
	{
	protected:
		EAttr Field = EAttr::Rotation;
		int32 RateDegrees = 0, RateMin = 0;
		float StopTime = 0.0f;

		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (RateDegrees == 0 || Ctx.DeltaTime <= 0.0f)
			{
				return;
			}
			const float Rate = RateDegrees * DegToRad;
			const float Floor = RateMin * DegToRad;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				float Effective = Rate;
				if (StopTime != 0.0f)
				{
					const float StopAge = StopTime * P.GetFloat(EAttr::LifeDuration, i);
					const float Ramp = StopAge > 0.0f ? Clamp01(1.0f - Age(P, Ctx, i) / StopAge) : 0.0f;
					Effective = Rate * Ramp;
					if (FMath::Abs(Effective) < FMath::Abs(Floor))
					{
						Effective = Floor * (Rate < 0.0f ? -1.0f : 1.0f);
					}
				}
				P.SetFloat(Field, i, P.GetFloat(Field, i) + Effective * Strength * Ctx.DeltaTime);
			}
		}
	};

	class FRotationSpinRoll : public FRotationSpin
	{
		virtual void Configure() override
		{
			Field = EAttr::Rotation;
			RateDegrees = I(TEXT("spin_rate_degrees"), 0);
			StopTime = F(TEXT("spin_stop_time"), 0.0f);
			RateMin = I(TEXT("spin_rate_min"), 0);
		}
	};

	class FRotationSpinYaw : public FRotationSpin
	{
		virtual void Configure() override
		{
			Field = EAttr::Yaw;
			RateDegrees = I(TEXT("yaw_rate_degrees"), 0);
			StopTime = F(TEXT("yaw_stop_time"), 0.0f);
			RateMin = I(TEXT("yaw_rate_min"), 0);
		}
	};

	/** Rotation Basic: ROTATION += ROTATION_SPEED * dt. */
	class FRotationBasic : public FSourceParticleOp
	{
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (Ctx.DeltaTime <= 0.0f)
			{
				return;
			}
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float Speed = P.GetFloat(EAttr::RotationSpeed, i);
				if (Speed != 0.0f)
				{
					P.SetFloat(EAttr::Rotation, i, P.GetFloat(EAttr::Rotation, i) + Speed * Strength * Ctx.DeltaTime);
				}
			}
		}
	};

	/** Rotation Orient to 2D Direction: a rotation field points along the particle's heading. */
	class FRotationOrientTo2DDirection : public FSourceParticleOp
	{
		float RotationOffset = 0.0f, SpinStrength = 1.0f;
		EAttr Field = EAttr::Rotation;
		virtual void Configure() override
		{
			RotationOffset = F(TEXT("Rotation Offset"), 0.0f);
			SpinStrength = F(TEXT("Spin Strength"), 1.0f);
			Field = SourceParticleAttr::FromField(I(TEXT("rotation field"), 4), EAttr::Rotation);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const float Blend = Clamp01(SpinStrength) * Strength;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const FVector3f Step = P.GetVector(EAttr::Xyz, i) - P.GetVector(EAttr::PrevXyz, i);
				if (Step.X * Step.X + Step.Y * Step.Y < 1e-10f)
				{
					continue;
				}
				const float Heading = FMath::Atan2(Step.Y, Step.X) + RotationOffset * DegToRad;
				const float Current = P.GetFloat(Field, i);
				P.SetFloat(Field, i, Current + (Heading - Current) * Blend);
			}
		}
	};

	/** Rotation Orient Relative to CP: a rotation field points at the control point. */
	class FRotationOrientRelativeToCP : public FSourceParticleOp
	{
		float RotationOffset = 0.0f, SpinStrength = 1.0f;
		int32 ControlPoint = 0;
		EAttr Field = EAttr::Rotation;
		virtual void Configure() override
		{
			RotationOffset = F(TEXT("Rotation Offset"), 0.0f);
			SpinStrength = F(TEXT("Spin Strength"), 1.0f);
			ControlPoint = I(TEXT("Control Point"), 0);
			Field = SourceParticleAttr::FromField(I(TEXT("rotation field"), 4), EAttr::Rotation);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const FVector3f CPPos = Ctx.CP(ControlPoint).Position;
			const float Blend = Clamp01(SpinStrength) * Strength;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const FVector3f ToCP = CPPos - P.GetVector(EAttr::Xyz, i);
				if (ToCP.X * ToCP.X + ToCP.Y * ToCP.Y < 1e-10f)
				{
					continue;
				}
				const float Heading = FMath::Atan2(ToCP.Y, ToCP.X) + RotationOffset * DegToRad;
				const float Current = P.GetFloat(Field, i);
				P.SetFloat(Field, i, Current + (Heading - Current) * Blend);
			}
		}
	};

	// ---- Field arithmetic ---------------------------------------------------------------------------------------

	/** Oscillate Scalar: rate * sin(pi * (mult * freq * t + phase)) * dt added to a field, in a random window. */
	class FOscillateScalar : public FSourceParticleOp
	{
		EAttr Field = EAttr::Alpha;
		float RateMin = 0.0f, RateMax = 0.0f, FreqMin = 1.0f, FreqMax = 1.0f, StartMin = 0.0f, StartMax = 0.0f, EndMin = 1.0f, EndMax = 1.0f;
		float Multiplier = 2.0f, StartPhase = 0.5f;
		bool bProportional = true, bStartEndProportional = true;
		virtual void Configure() override
		{
			Field = SourceParticleAttr::FromField(I(TEXT("oscillation field"), 7), EAttr::Alpha);
			RateMin = F(TEXT("oscillation rate min"), 0.0f);
			RateMax = F(TEXT("oscillation rate max"), 0.0f);
			FreqMin = F(TEXT("oscillation frequency min"), 1.0f);
			FreqMax = F(TEXT("oscillation frequency max"), 1.0f);
			bProportional = B(TEXT("proportional 0/1"), true);
			StartMin = F(TEXT("start time min"), 0.0f);
			StartMax = F(TEXT("start time max"), 0.0f);
			EndMin = F(TEXT("end time min"), 1.0f);
			EndMax = F(TEXT("end time max"), 1.0f);
			bStartEndProportional = B(TEXT("start/end proportional"), true);
			Multiplier = F(TEXT("oscillation multiplier"), 2.0f);
			StartPhase = F(TEXT("oscillation start phase"), 0.5f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const float Dt = Ctx.DeltaTime;
			if (Dt <= 0.0f)
			{
				return;
			}
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float A = Age(P, Ctx, i);
				const float Life = P.GetFloat(EAttr::LifeDuration, i);
				const float WindowTime = (bStartEndProportional && Life > 0.0f) ? A / Life : A;
				const float Start = StableRandRangeExp(P, i, 0, StartMin, StartMax, 1.0f);
				const float End = StableRandRangeExp(P, i, 3, EndMin, EndMax, 1.0f);
				if (WindowTime < Start || WindowTime > End)
				{
					continue;
				}
				const float Rate = StableRandRangeExp(P, i, 6, RateMin, RateMax, 1.0f);
				const float Frequency = StableRandRangeExp(P, i, 9, FreqMin, FreqMax, 1.0f);
				const float T = (bProportional && Life > 0.0f) ? A / Life : Ctx.Time;
				const float Wave = FMath::Sin(PI * (Multiplier * Frequency * T + StartPhase));
				P.SetFloat(Field, i, P.GetFloat(Field, i) + Rate * Wave * Dt * Strength);
			}
		}
	};

	/** Oscillate Vector: the per-axis version. */
	class FOscillateVector : public FSourceParticleOp
	{
		EAttr Field = EAttr::Xyz;
		FVector3f RateMin = FVector3f::ZeroVector, RateMax = FVector3f::ZeroVector, FreqMin = FVector3f(1, 1, 1), FreqMax = FVector3f(1, 1, 1);
		float StartMin = 0.0f, StartMax = 0.0f, EndMin = 1.0f, EndMax = 1.0f, Multiplier = 2.0f, StartPhase = 0.5f;
		bool bProportional = true, bStartEndProportional = true;
		virtual void Configure() override
		{
			Field = SourceParticleAttr::FromField(I(TEXT("oscillation field"), 0), EAttr::Xyz);
			RateMin = V(TEXT("oscillation rate min"), FVector3f::ZeroVector);
			RateMax = V(TEXT("oscillation rate max"), FVector3f::ZeroVector);
			FreqMin = V(TEXT("oscillation frequency min"), FVector3f(1, 1, 1));
			FreqMax = V(TEXT("oscillation frequency max"), FVector3f(1, 1, 1));
			bProportional = B(TEXT("proportional 0/1"), true);
			StartMin = F(TEXT("start time min"), 0.0f);
			StartMax = F(TEXT("start time max"), 0.0f);
			EndMin = F(TEXT("end time min"), 1.0f);
			EndMax = F(TEXT("end time max"), 1.0f);
			bStartEndProportional = B(TEXT("start/end proportional"), true);
			Multiplier = F(TEXT("oscillation multiplier"), 2.0f);
			StartPhase = F(TEXT("oscillation start phase"), 0.5f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const float Dt = Ctx.DeltaTime;
			if (Dt <= 0.0f)
			{
				return;
			}
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float A = Age(P, Ctx, i);
				const float Life = P.GetFloat(EAttr::LifeDuration, i);
				const float WindowTime = (bStartEndProportional && Life > 0.0f) ? A / Life : A;
				const float Start = StableRandRangeExp(P, i, 0, StartMin, StartMax, 1.0f);
				const float End = StableRandRangeExp(P, i, 3, EndMin, EndMax, 1.0f);
				if (WindowTime < Start || WindowTime > End)
				{
					continue;
				}
				const float T = (bProportional && Life > 0.0f) ? A / Life : Ctx.Time;
				FVector3f Value = P.GetVector(Field, i);
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					const float Rate = StableRandRangeExp(P, i, 6 + Axis, Component(RateMin, Axis), Component(RateMax, Axis), 1.0f);
					const float Frequency = StableRandRangeExp(P, i, 9 + Axis, Component(FreqMin, Axis), Component(FreqMax, Axis), 1.0f);
					const float Wave = FMath::Sin(PI * (Multiplier * Frequency * T + StartPhase));
					Value[Axis] += Rate * Wave * Dt * Strength;
				}
				P.SetVector(Field, i, Value);
			}
		}
	};

	/** Oscillate Scalar Simple. */
	class FOscillateScalarSimple : public FSourceParticleOp
	{
		EAttr Field = EAttr::Alpha;
		float Rate = 0.0f, Frequency = 1.0f, Multiplier = 2.0f, StartPhase = 0.5f;
		virtual void Configure() override
		{
			Field = SourceParticleAttr::FromField(I(TEXT("oscillation field"), 7), EAttr::Alpha);
			Rate = F(TEXT("oscillation rate"), 0.0f);
			Frequency = F(TEXT("oscillation frequency"), 1.0f);
			Multiplier = F(TEXT("oscillation multiplier"), 2.0f);
			StartPhase = F(TEXT("oscillation start phase"), 0.5f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const float Dt = Ctx.DeltaTime;
			if (Dt <= 0.0f || Rate == 0.0f)
			{
				return;
			}
			const float Delta = Rate * FMath::Sin(PI * (Multiplier * Frequency * Ctx.Time + StartPhase)) * Dt * Strength;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				P.SetFloat(Field, i, P.GetFloat(Field, i) + Delta);
			}
		}
	};

	/** Oscillate Vector Simple. */
	class FOscillateVectorSimple : public FSourceParticleOp
	{
		EAttr Field = EAttr::Xyz;
		FVector3f Rate = FVector3f::ZeroVector, Frequency = FVector3f(1, 1, 1);
		float Multiplier = 2.0f, StartPhase = 0.5f;
		virtual void Configure() override
		{
			Field = SourceParticleAttr::FromField(I(TEXT("oscillation field"), 0), EAttr::Xyz);
			Rate = V(TEXT("oscillation rate"), FVector3f::ZeroVector);
			Frequency = V(TEXT("oscillation frequency"), FVector3f(1, 1, 1));
			Multiplier = F(TEXT("oscillation multiplier"), 2.0f);
			StartPhase = F(TEXT("oscillation start phase"), 0.5f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const float Dt = Ctx.DeltaTime;
			if (Dt <= 0.0f)
			{
				return;
			}
			const float Arg = PI * Multiplier;
			const FVector3f Delta = FVector3f(
				Rate.X * FMath::Sin(Arg * Frequency.X * Ctx.Time + PI * StartPhase),
				Rate.Y * FMath::Sin(Arg * Frequency.Y * Ctx.Time + PI * StartPhase),
				Rate.Z * FMath::Sin(Arg * Frequency.Z * Ctx.Time + PI * StartPhase)) * Dt * Strength;
			if (Delta.IsZero())
			{
				return;
			}
			for (int32 i = 0; i < P.Num(); ++i)
			{
				P.SetVector(Field, i, P.GetVector(Field, i) + Delta);
			}
		}
	};

	/** Noise Scalar: a per-particle noise value that wanders with time. */
	class FNoiseScalar : public FSourceParticleOp
	{
		EAttr Field = EAttr::Radius;
		float OutMin = 0.0f, OutMax = 1.0f;
		bool bAdditive = false;
		virtual void Configure() override
		{
			Field = SourceParticleAttr::FromField(I(TEXT("output field"), 3), EAttr::Radius);
			OutMin = F(TEXT("output minimum"), 0.0f);
			OutMax = F(TEXT("output maximum"), 1.0f);
			bAdditive = B(TEXT("additive"), false);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float T = Noise1D(P.IdOf(i) * 0.918f + Ctx.Time + InstanceSeed) * 0.5f + 0.5f;
				const float Value = Lerp(OutMin, OutMax, T);
				P.SetFloat(Field, i, bAdditive ? P.GetFloat(Field, i) + Value * Ctx.DeltaTime : Value);
			}
		}
	};

	/** Noise Vector. */
	class FNoiseVector : public FSourceParticleOp
	{
		EAttr Field = EAttr::TintRgb;
		FVector3f OutMin = FVector3f::ZeroVector, OutMax = FVector3f(1, 1, 1);
		bool bAdditive = false;
		virtual void Configure() override
		{
			Field = SourceParticleAttr::FromField(I(TEXT("output field"), 6), EAttr::TintRgb);
			OutMin = V(TEXT("output minimum"), FVector3f::ZeroVector);
			OutMax = V(TEXT("output maximum"), FVector3f(1, 1, 1));
			bAdditive = B(TEXT("additive"), false);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const FVector3f Noise = Noise3D(FVector3f(P.IdOf(i) * 0.918f + InstanceSeed, Ctx.Time, 0.0f));
				const FVector3f Value(Lerp(OutMin.X, OutMax.X, Noise.X * 0.5f + 0.5f), Lerp(OutMin.Y, OutMax.Y, Noise.Y * 0.5f + 0.5f),
					Lerp(OutMin.Z, OutMax.Z, Noise.Z * 0.5f + 0.5f));
				P.SetVector(Field, i, bAdditive ? P.GetVector(Field, i) + Value * Ctx.DeltaTime : Value);
			}
		}
	};

	/** Remap Scalar. */
	class FRemapScalar : public FSourceParticleOp
	{
		EAttr Input = EAttr::Alpha, Output = EAttr::Radius;
		float InMin = 0.0f, InMax = 1.0f, OutMin = 0.0f, OutMax = 1.0f;
		virtual void Configure() override
		{
			Input = SourceParticleAttr::FromField(I(TEXT("input field"), 7), EAttr::Alpha);
			InMin = F(TEXT("input minimum"), 0.0f);
			InMax = F(TEXT("input maximum"), 1.0f);
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 3), EAttr::Radius);
			OutMin = F(TEXT("output minimum"), 0.0f);
			OutMax = F(TEXT("output maximum"), 1.0f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			for (int32 i = 0; i < P.Num(); ++i)
			{
				P.SetFloat(Output, i, Lerp(OutMin, OutMax, RemapClamped(P.GetFloat(Input, i), InMin, InMax)));
			}
		}
	};

	/** Clamp Scalar. */
	class FClampScalar : public FSourceParticleOp
	{
		EAttr Field = EAttr::Radius;
		float Min = 0.0f, Max = 1.0f;
		virtual void Configure() override
		{
			Field = SourceParticleAttr::FromField(I(TEXT("output field"), 3), EAttr::Radius);
			Min = F(TEXT("output minimum"), 0.0f);
			Max = F(TEXT("output maximum"), 1.0f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			for (int32 i = 0; i < P.Num(); ++i)
			{
				P.SetFloat(Field, i, FMath::Clamp(P.GetFloat(Field, i), Min, Max));
			}
		}
	};

	/** Clamp Vector. */
	class FClampVector : public FSourceParticleOp
	{
		EAttr Field = EAttr::Xyz;
		FVector3f Min = FVector3f::ZeroVector, Max = FVector3f(1, 1, 1);
		virtual void Configure() override
		{
			Field = SourceParticleAttr::FromField(I(TEXT("output field"), 0), EAttr::Xyz);
			Min = V(TEXT("output minimum"), FVector3f::ZeroVector);
			Max = V(TEXT("output maximum"), FVector3f(1, 1, 1));
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			for (int32 i = 0; i < P.Num(); ++i)
			{
				P.SetVector(Field, i, FVector3f::Min(FVector3f::Max(P.GetVector(Field, i), Min), Max));
			}
		}
	};

	/** Normalize Vector. */
	class FNormalizeVector : public FSourceParticleOp
	{
		EAttr Field = EAttr::Xyz;
		float Scale = 1.0f;
		virtual void Configure() override
		{
			Field = SourceParticleAttr::FromField(I(TEXT("output field"), 0), EAttr::Xyz);
			Scale = F(TEXT("scale factor"), 1.0f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const FVector3f Value = P.GetVector(Field, i);
				if (Value.SizeSquared() > 1e-8f)
				{
					P.SetVector(Field, i, Value.GetSafeNormal() * Scale);
				}
			}
		}
	};

	/** Lerp Initial Scalar: from the spawn value towards a target over a life window. */
	class FLerpInitialScalar : public FSourceParticleOp
	{
		float StartTime = 0.0f, EndTime = 1.0f, Target = 1.0f;
		EAttr Field = EAttr::Radius;
		virtual void Configure() override
		{
			StartTime = F(TEXT("start time"), 0.0f);
			EndTime = F(TEXT("end time"), 1.0f);
			Field = SourceParticleAttr::FromField(I(TEXT("output field"), 3), EAttr::Radius);
			Target = F(TEXT("value to lerp to"), 1.0f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float T = RemapClamped(LifeFraction(P, Ctx, i), StartTime, EndTime);
				P.SetFloat(Field, i, Lerp(P.GetInitialFloat(Field, i), Target, T));
			}
		}
	};

	/** Lerp Initial Vector. */
	class FLerpInitialVector : public FSourceParticleOp
	{
		float StartTime = 0.0f, EndTime = 1.0f;
		FVector3f Target = FVector3f::ZeroVector;
		EAttr Field = EAttr::Xyz;
		virtual void Configure() override
		{
			StartTime = F(TEXT("start time"), 0.0f);
			EndTime = F(TEXT("end time"), 1.0f);
			Field = SourceParticleAttr::FromField(I(TEXT("output field"), 0), EAttr::Xyz);
			Target = V(TEXT("value to lerp to"), FVector3f::ZeroVector);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float T = RemapClamped(LifeFraction(P, Ctx, i), StartTime, EndTime);
				P.SetVector(Field, i, FMath::Lerp(P.GetInitialVector(Field, i), Target, T));
			}
		}
	};

	/** Lerp EndCap Scalar: once the end cap starts, a field lerps to a target over "lerp time". */
	class FLerpEndCapScalar : public FSourceParticleOp
	{
		float LerpTime = 1.0f, Target = 1.0f, Elapsed = 0.0f;
		EAttr Field = EAttr::Radius;
		virtual void Configure() override
		{
			LerpTime = F(TEXT("lerp time"), 1.0f);
			Field = SourceParticleAttr::FromField(I(TEXT("output field"), 3), EAttr::Radius);
			Target = F(TEXT("value to lerp to"), 1.0f);
		}
		virtual void Reset() override { Elapsed = 0.0f; }
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (!Ctx.bInEndCap || Ctx.DeltaTime <= 0.0f)
			{
				return;
			}
			const float Remaining = LerpTime - Elapsed;
			const float Step = (Remaining <= Ctx.DeltaTime || LerpTime <= 0.0f) ? 1.0f : Ctx.DeltaTime / Remaining;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float Value = P.GetFloat(Field, i);
				P.SetFloat(Field, i, Value + (Target - Value) * Step);
			}
			Elapsed += Ctx.DeltaTime;
		}
	};

	/** Lerp EndCap Vector. */
	class FLerpEndCapVector : public FSourceParticleOp
	{
		float LerpTime = 1.0f, Elapsed = 0.0f;
		FVector3f Target = FVector3f::ZeroVector;
		EAttr Field = EAttr::Xyz;
		virtual void Configure() override
		{
			LerpTime = F(TEXT("lerp time"), 1.0f);
			Field = SourceParticleAttr::FromField(I(TEXT("output field"), 0), EAttr::Xyz);
			Target = V(TEXT("value to lerp to"), FVector3f::ZeroVector);
		}
		virtual void Reset() override { Elapsed = 0.0f; }
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (!Ctx.bInEndCap || Ctx.DeltaTime <= 0.0f)
			{
				return;
			}
			const float Remaining = LerpTime - Elapsed;
			const float Step = (Remaining <= Ctx.DeltaTime || LerpTime <= 0.0f) ? 1.0f : Ctx.DeltaTime / Remaining;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const FVector3f Value = P.GetVector(Field, i);
				P.SetVector(Field, i, Value + (Target - Value) * Step);
			}
			Elapsed += Ctx.DeltaTime;
		}
	};

	/** The ramps: field += rate * dt inside a window, with the random and spline variants. */
	class FRampScalarLinearSimple : public FSourceParticleOp
	{
	protected:
		EAttr Field = EAttr::Radius;
		float Rate = 0.0f, StartTime = 0.0f, EndTime = 1.0f;
		bool bEaseOut = false;
		virtual void Configure() override
		{
			Field = SourceParticleAttr::FromField(I(TEXT("ramp field"), 3), EAttr::Radius);
			Rate = F(TEXT("ramp rate"), 0.0f);
			StartTime = F(TEXT("start time"), 0.0f);
			EndTime = F(TEXT("end time"), 1.0f);
			bEaseOut = B(TEXT("ease out"), false);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const float Dt = Ctx.DeltaTime;
			if (Dt <= 0.0f || Rate == 0.0f)
			{
				return;
			}
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float A = Age(P, Ctx, i);
				if (A < StartTime || A > EndTime)
				{
					continue;
				}
				float R = Rate;
				if (bEaseOut && EndTime > StartTime)
				{
					R *= 1.0f - SimpleSpline(RemapClamped(A, StartTime, EndTime));
				}
				P.SetFloat(Field, i, P.GetFloat(Field, i) + R * Dt * Strength);
			}
		}
	};

	class FRampScalarLinearRandom : public FSourceParticleOp
	{
	protected:
		EAttr Field = EAttr::Radius;
		float RateMin = 0.0f, RateMax = 0.0f, StartMin = 0.0f, StartMax = 0.0f, EndMin = 1.0f, EndMax = 1.0f, BiasValue = 0.5f;
		bool bProportional = true, bEaseOut = false;
		virtual void Configure() override
		{
			Field = SourceParticleAttr::FromField(I(TEXT("ramp field"), 3), EAttr::Radius);
			RateMin = F(TEXT("ramp rate min"), 0.0f);
			RateMax = F(TEXT("ramp rate max"), 0.0f);
			StartMin = F(TEXT("start time min"), 0.0f);
			StartMax = F(TEXT("start time max"), 0.0f);
			EndMin = F(TEXT("end time min"), 1.0f);
			EndMax = F(TEXT("end time max"), 1.0f);
			bProportional = B(TEXT("start/end proportional"), true);
			bEaseOut = B(TEXT("ease out"), false);
			BiasValue = F(TEXT("bias"), 0.5f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const float Dt = Ctx.DeltaTime;
			if (Dt <= 0.0f)
			{
				return;
			}
			for (int32 i = 0; i < P.Num(); ++i)
			{
				float R = StableRandRangeExp(P, i, 0, RateMin, RateMax, 1.0f);
				if (R == 0.0f)
				{
					continue;
				}
				const float Start = StableRandRangeExp(P, i, 3, StartMin, StartMax, 1.0f);
				const float End = StableRandRangeExp(P, i, 6, EndMin, EndMax, 1.0f);
				const float WindowTime = bProportional ? LifeFraction(P, Ctx, i) : Age(P, Ctx, i);
				if (WindowTime < Start || WindowTime > End)
				{
					continue;
				}
				if (bEaseOut && End > Start)
				{
					R *= 1.0f - FadeCurve(RemapClamped(WindowTime, Start, End), true, BiasValue);
				}
				P.SetFloat(Field, i, P.GetFloat(Field, i) + R * Dt * Strength);
			}
		}
	};

	// ---- Control-point remaps and writers -----------------------------------------------------------------------

	/** The "output is scalar of ..." choice shared by the remap-to-scalar operators. */
	static float ApplyScalarMode(const FSourceParticleCollection& P, EAttr Output, int32 Particle, float Value, bool bScaleInitial, bool bScaleCurrent)
	{
		if (bScaleInitial)
		{
			return Value * P.GetInitialFloat(Output, Particle);
		}
		if (bScaleCurrent)
		{
			return Value * P.GetFloat(Output, Particle);
		}
		return Value;
	}

	/** Remap Distance to Control Point to Scalar. Line of sight is not simulated. */
	class FRemapDistanceToControlPointToScalar : public FSourceParticleOp
	{
		float DistMin = 0.0f, DistMax = 128.0f, OutMin = 0.0f, OutMax = 1.0f;
		EAttr Output = EAttr::Radius;
		int32 ControlPoint = 0;
		bool bScaleInitial = false, bScaleCurrent = false, bOnlyInRange = false;
		virtual void Configure() override
		{
			DistMin = F(TEXT("distance minimum"), 0.0f);
			DistMax = F(TEXT("distance maximum"), 128.0f);
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 3), EAttr::Radius);
			OutMin = F(TEXT("output minimum"), 0.0f);
			OutMax = F(TEXT("output maximum"), 1.0f);
			ControlPoint = I(TEXT("control point"), 0);
			bScaleInitial = B(TEXT("output is scalar of initial random range"), false);
			bScaleCurrent = B(TEXT("output is scalar of current value"), false);
			bOnlyInRange = B(TEXT("only active within specified distance"), false);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const FVector3f CPPos = Ctx.CP(ControlPoint).Position;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float Dist = (P.GetVector(EAttr::Xyz, i) - CPPos).Size();
				if (bOnlyInRange && (Dist < DistMin || Dist > DistMax))
				{
					continue;
				}
				const float Result = Lerp(OutMin, OutMax, RemapClamped(Dist, DistMin, DistMax));
				P.SetFloat(Output, i, ApplyScalarMode(P, Output, i, Result, bScaleInitial, bScaleCurrent));
			}
		}
	};

	/** Remap Control Point to Scalar (operator). */
	class FRemapControlPointToScalarOp : public FSourceParticleOp
	{
		int32 InputCP = 0, InputField = 0;
		float InMin = 0.0f, InMax = 1.0f, OutMin = 0.0f, OutMax = 1.0f;
		EAttr Output = EAttr::Radius;
		bool bScaleInitial = false, bScaleCurrent = false;
		virtual void Configure() override
		{
			InputCP = I(TEXT("input control point number"), 0);
			InMin = F(TEXT("input minimum"), 0.0f);
			InMax = F(TEXT("input maximum"), 1.0f);
			InputField = I(TEXT("input field 0-2 X/Y/Z"), 0);
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 3), EAttr::Radius);
			OutMin = F(TEXT("output minimum"), 0.0f);
			OutMax = F(TEXT("output maximum"), 1.0f);
			bScaleInitial = B(TEXT("output is scalar of initial random range"), false);
			bScaleCurrent = B(TEXT("output is scalar of current value"), false);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const float Result = Lerp(OutMin, OutMax, RemapClamped(Ctx.CPField(InputCP, InputField), InMin, InMax));
			for (int32 i = 0; i < P.Num(); ++i)
			{
				P.SetFloat(Output, i, ApplyScalarMode(P, Output, i, Result, bScaleInitial, bScaleCurrent));
			}
		}
	};

	/** Remap Control Point to Vector (operator). The offset/accelerate-position modes are not simulated. */
	class FRemapControlPointToVectorOp : public FSourceParticleOp
	{
		int32 InputCP = 0;
		FVector3f InMin = FVector3f::ZeroVector, InMax = FVector3f::ZeroVector, OutMin = FVector3f::ZeroVector, OutMax = FVector3f::ZeroVector;
		EAttr Output = EAttr::Xyz;
		bool bOffsetPosition = false, bAccelerate = false;
		virtual void Configure() override
		{
			InputCP = I(TEXT("input control point number"), 0);
			InMin = V(TEXT("input minimum"), FVector3f::ZeroVector);
			InMax = V(TEXT("input maximum"), FVector3f::ZeroVector);
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 0), EAttr::Xyz);
			OutMin = V(TEXT("output minimum"), FVector3f::ZeroVector);
			OutMax = V(TEXT("output maximum"), FVector3f::ZeroVector);
			bOffsetPosition = B(TEXT("offset position"), false);
			bAccelerate = B(TEXT("accelerate position"), false);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (bOffsetPosition || bAccelerate)
			{
				return;
			}
			const FVector3f Pos = Ctx.CP(InputCP).Position;
			const FVector3f Result(Lerp(OutMin.X, OutMax.X, RemapClamped(Pos.X, InMin.X, InMax.X)),
				Lerp(OutMin.Y, OutMax.Y, RemapClamped(Pos.Y, InMin.Y, InMax.Y)),
				Lerp(OutMin.Z, OutMax.Z, RemapClamped(Pos.Z, InMin.Z, InMax.Z)));
			for (int32 i = 0; i < P.Num(); ++i)
			{
				P.SetVector(Output, i, Result);
			}
		}
	};

	/** Remap Velocity to Vector. */
	class FRemapVelocityToVector : public FSourceParticleOp
	{
		EAttr Output = EAttr::Xyz;
		bool bNormalize = false;
		float Scale = 1.0f;
		virtual void Configure() override
		{
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 0), EAttr::Xyz);
			bNormalize = B(TEXT("normalize"), false);
			Scale = F(TEXT("scale factor"), 1.0f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (Ctx.DeltaTime <= 0.0f)
			{
				return;
			}
			for (int32 i = 0; i < P.Num(); ++i)
			{
				FVector3f Velocity = (P.GetVector(EAttr::Xyz, i) - P.GetVector(EAttr::PrevXyz, i)) / Ctx.DeltaTime;
				if (bNormalize)
				{
					Velocity = Velocity.GetSafeNormal();
				}
				P.SetVector(Output, i, Velocity * Scale);
			}
		}
	};

	/** Remap Speed to Scalar (operator). */
	class FRemapSpeedToScalarOp : public FSourceParticleOp
	{
		float InMin = 0.0f, InMax = 1.0f, OutMin = 0.0f, OutMax = 1.0f;
		EAttr Output = EAttr::Radius;
		bool bScaleInitial = false, bScaleCurrent = false;
		virtual void Configure() override
		{
			InMin = F(TEXT("input minimum"), 0.0f);
			InMax = F(TEXT("input maximum"), 1.0f);
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 3), EAttr::Radius);
			OutMin = F(TEXT("output minimum"), 0.0f);
			OutMax = F(TEXT("output maximum"), 1.0f);
			bScaleInitial = B(TEXT("output is scalar of initial random range"), false);
			bScaleCurrent = B(TEXT("output is scalar of current value"), false);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (Ctx.DeltaTime <= 0.0f)
			{
				return;
			}
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float Speed = ((P.GetVector(EAttr::Xyz, i) - P.GetVector(EAttr::PrevXyz, i)) / Ctx.DeltaTime).Size();
				const float Result = Lerp(OutMin, OutMax, RemapClamped(Speed, InMin, InMax));
				P.SetFloat(Output, i, ApplyScalarMode(P, Output, i, Result, bScaleInitial, bScaleCurrent));
			}
		}
	};

	/** Remap Distance Between Two Control Points to Scalar. */
	class FRemapDistanceBetweenTwoControlPointsToScalar : public FSourceParticleOp
	{
		float DistMin = 0.0f, DistMax = 128.0f, OutMin = 0.0f, OutMax = 1.0f;
		EAttr Output = EAttr::Radius;
		int32 StartCP = 0, EndCP = 1;
		bool bScaleInitial = false, bScaleCurrent = false;
		virtual void Configure() override
		{
			DistMin = F(TEXT("distance minimum"), 0.0f);
			DistMax = F(TEXT("distance maximum"), 128.0f);
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 3), EAttr::Radius);
			OutMin = F(TEXT("output minimum"), 0.0f);
			OutMax = F(TEXT("output maximum"), 1.0f);
			StartCP = I(TEXT("starting control point"), 0);
			EndCP = I(TEXT("ending control point"), 1);
			bScaleInitial = B(TEXT("output is scalar of initial random range"), false);
			bScaleCurrent = B(TEXT("output is scalar of current value"), false);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const float Distance = (Ctx.CP(EndCP).Position - Ctx.CP(StartCP).Position).Size();
			const float Result = Lerp(OutMin, OutMax, RemapClamped(Distance, DistMin, DistMax));
			for (int32 i = 0; i < P.Num(); ++i)
			{
				P.SetFloat(Output, i, ApplyScalarMode(P, Output, i, Result, bScaleInitial, bScaleCurrent));
			}
		}
	};

	/** The percentage of the way from one control point to another that a particle sits at. */
	static float PercentageBetween(const FVector3f& Pos, const FVector3f& Start, const FVector3f& Axis, bool bRadius)
	{
		const FVector3f Rel = Pos - Start;
		const float LengthSq = Axis.SizeSquared();
		if (bRadius)
		{
			const float Length = FMath::Sqrt(LengthSq);
			return Length > 1e-6f ? Rel.Size() / Length : 0.0f;
		}
		return LengthSq > 1e-8f ? FVector3f::DotProduct(Rel, Axis) / LengthSq : 0.0f;
	}

	/** Remap Percentage Between Two Control Points to Scalar. */
	class FRemapPercentageBetweenTwoControlPointsToScalar : public FSourceParticleOp
	{
		float PctMin = 0.0f, PctMax = 1.0f, OutMin = 0.0f, OutMax = 1.0f;
		EAttr Output = EAttr::Radius;
		int32 StartCP = 0, EndCP = 1;
		bool bScaleInitial = false, bScaleCurrent = false, bOnlyInRange = false, bRadius = true;
		virtual void Configure() override
		{
			PctMin = F(TEXT("percentage minimum"), 0.0f);
			PctMax = F(TEXT("percentage maximum"), 1.0f);
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 3), EAttr::Radius);
			OutMin = F(TEXT("output minimum"), 0.0f);
			OutMax = F(TEXT("output maximum"), 1.0f);
			StartCP = I(TEXT("starting control point"), 0);
			EndCP = I(TEXT("ending control point"), 1);
			bScaleInitial = B(TEXT("output is scalar of initial random range"), false);
			bScaleCurrent = B(TEXT("output is scalar of current value"), false);
			bOnlyInRange = B(TEXT("only active within input range"), false);
			bRadius = B(TEXT("treat distance between points as radius"), true);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const FVector3f Start = Ctx.CP(StartCP).Position;
			const FVector3f Axis = Ctx.CP(EndCP).Position - Start;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float Pct = PercentageBetween(P.GetVector(EAttr::Xyz, i), Start, Axis, bRadius);
				if (bOnlyInRange && (Pct < PctMin || Pct > PctMax))
				{
					continue;
				}
				const float Result = Lerp(OutMin, OutMax, RemapClamped(Pct, PctMin, PctMax));
				P.SetFloat(Output, i, ApplyScalarMode(P, Output, i, Result, bScaleInitial, bScaleCurrent));
			}
		}
	};

	/** Remap Percentage Between Two Control Points to Vector. */
	class FRemapPercentageBetweenTwoControlPointsToVector : public FSourceParticleOp
	{
		float PctMin = 0.0f, PctMax = 1.0f;
		FVector3f OutMin = FVector3f::ZeroVector, OutMax = FVector3f(1, 1, 1);
		EAttr Output = EAttr::TintRgb;
		int32 StartCP = 0, EndCP = 1;
		bool bScaleInitial = false, bScaleCurrent = false, bOnlyInRange = false, bRadius = true;
		virtual void Configure() override
		{
			PctMin = F(TEXT("percentage minimum"), 0.0f);
			PctMax = F(TEXT("percentage maximum"), 1.0f);
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 6), EAttr::TintRgb);
			OutMin = V(TEXT("output minimum"), FVector3f::ZeroVector);
			OutMax = V(TEXT("output maximum"), FVector3f(1, 1, 1));
			StartCP = I(TEXT("starting control point"), 0);
			EndCP = I(TEXT("ending control point"), 1);
			bScaleInitial = B(TEXT("output is scalar of initial random range"), false);
			bScaleCurrent = B(TEXT("output is scalar of current value"), false);
			bOnlyInRange = B(TEXT("only active within input range"), false);
			bRadius = B(TEXT("treat distance between points as radius"), true);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const FVector3f Start = Ctx.CP(StartCP).Position;
			const FVector3f Axis = Ctx.CP(EndCP).Position - Start;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float Pct = PercentageBetween(P.GetVector(EAttr::Xyz, i), Start, Axis, bRadius);
				if (bOnlyInRange && (Pct < PctMin || Pct > PctMax))
				{
					continue;
				}
				FVector3f Value = FMath::Lerp(OutMin, OutMax, RemapClamped(Pct, PctMin, PctMax));
				if (bScaleInitial)
				{
					Value *= P.GetInitialVector(Output, i);
				}
				else if (bScaleCurrent)
				{
					Value *= P.GetVector(Output, i);
				}
				P.SetVector(Output, i, Value);
			}
		}
	};

	/** Remap Dot Product to Scalar. */
	class FRemapDotProductToScalar : public FSourceParticleOp
	{
		bool bUseVelocity = false, bScaleInitial = false, bScaleCurrent = false, bOnlyInRange = false;
		int32 FirstCP = 0, SecondCP = 0;
		float InMin = 0.0f, InMax = 1.0f, OutMin = 0.0f, OutMax = 1.0f;
		EAttr Output = EAttr::Radius;
		virtual void Configure() override
		{
			bUseVelocity = B(TEXT("use particle velocity for first input"), false);
			FirstCP = I(TEXT("first input control point"), 0);
			SecondCP = I(TEXT("second input control point"), 0);
			InMin = F(TEXT("input minimum (-1 to 1)"), 0.0f);
			InMax = F(TEXT("input maximum (-1 to 1)"), 1.0f);
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 3), EAttr::Radius);
			OutMin = F(TEXT("output minimum"), 0.0f);
			OutMax = F(TEXT("output maximum"), 1.0f);
			bScaleInitial = B(TEXT("output is scalar of initial random range"), false);
			bScaleCurrent = B(TEXT("output is scalar of current value"), false);
			bOnlyInRange = B(TEXT("only active within specified input range"), false);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const FVector3f V2 = Ctx.CP(SecondCP).Forward;
			const FVector3f CPForward = Ctx.CP(FirstCP).Forward;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				FVector3f V1 = CPForward;
				if (bUseVelocity && Ctx.DeltaTime > 0.0f)
				{
					V1 = ((P.GetVector(EAttr::Xyz, i) - P.GetVector(EAttr::PrevXyz, i)) / Ctx.DeltaTime).GetSafeNormal();
				}
				const float Dot = FVector3f::DotProduct(V1, V2);
				if (bOnlyInRange && (Dot < InMin || Dot > InMax))
				{
					continue;
				}
				const float Result = Lerp(OutMin, OutMax, RemapClamped(Dot, InMin, InMax));
				P.SetFloat(Output, i, ApplyScalarMode(P, Output, i, Result, bScaleInitial, bScaleCurrent));
			}
		}
	};

	/** Rotate Vector Random: a vector field spun about a random axis at a random rate. */
	class FRotateVectorRandom : public FSourceParticleOp
	{
		EAttr Field = EAttr::Normal;
		FVector3f AxisMin = FVector3f(0, 0, 1), AxisMax = FVector3f(0, 0, 1);
		float RateMin = 180.0f, RateMax = 180.0f, NormalizeOutput = 0.0f;
		virtual void Configure() override
		{
			Field = SourceParticleAttr::FromField(I(TEXT("output field"), 21), EAttr::Normal);
			AxisMin = V(TEXT("Rotation Axis Min"), FVector3f(0, 0, 1));
			AxisMax = V(TEXT("Rotation Axis Max"), FVector3f(0, 0, 1));
			RateMin = F(TEXT("Rotation Rate Min"), 180.0f);
			RateMax = F(TEXT("Rotation Rate Max"), 180.0f);
			NormalizeOutput = F(TEXT("Normalize Ouput"), 0.0f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (Ctx.DeltaTime <= 0.0f)
			{
				return;
			}
			for (int32 i = 0; i < P.Num(); ++i)
			{
				FVector3f Axis(StableRandRangeExp(P, i, 0, AxisMin.X, AxisMax.X, 1.0f), StableRandRangeExp(P, i, 1, AxisMin.Y, AxisMax.Y, 1.0f),
					StableRandRangeExp(P, i, 2, AxisMin.Z, AxisMax.Z, 1.0f));
				if (Axis.SizeSquared() < 1e-8f)
				{
					continue;
				}
				Axis.Normalize();
				const float Rate = StableRandRangeExp(P, i, 3, RateMin, RateMax, 1.0f);
				FVector3f Value = RotateAboutAxis(P.GetVector(Field, i), Axis, Rate * DegToRad * Ctx.DeltaTime * Strength);
				if (NormalizeOutput != 0.0f && Value.SizeSquared() > 1e-8f)
				{
					Value.Normalize();
				}
				P.SetVector(Field, i, Value);
			}
		}
	};

	/** Remap Control Point Direction to Vector. */
	class FRemapControlPointDirectionToVector : public FSourceParticleOp
	{
		EAttr Output = EAttr::Xyz;
		int32 ControlPoint = 0;
		float Scale = 1.0f;
		virtual void Configure() override
		{
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 0), EAttr::Xyz);
			ControlPoint = I(TEXT("control point number"), 0);
			Scale = F(TEXT("scale factor"), 1.0f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const FVector3f Result = Ctx.CP(ControlPoint).Forward.GetSafeNormal() * Scale;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				P.SetVector(Output, i, Result);
			}
		}
	};

	/** Remap CP Velocity to Vector. */
	class FRemapCPVelocityToVector : public FSourceParticleOp
	{
		EAttr Output = EAttr::Xyz;
		int32 ControlPoint = 0;
		bool bNormalize = false;
		float Scale = 1.0f;
		virtual void Configure() override
		{
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 0), EAttr::Xyz);
			ControlPoint = I(TEXT("control point"), 0);
			bNormalize = B(TEXT("normalize"), false);
			Scale = F(TEXT("scale factor"), 1.0f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			FVector3f Velocity = Ctx.CP(ControlPoint).Velocity(Ctx.DeltaTime);
			if (bNormalize)
			{
				Velocity = Velocity.GetSafeNormal();
			}
			Velocity *= Scale;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				P.SetVector(Output, i, Velocity);
			}
		}
	};

	/** Remap Direction to CP to Vector. */
	class FRemapDirectionToCPToVector : public FSourceParticleOp
	{
		int32 ControlPoint = 0;
		EAttr Output = EAttr::Xyz;
		bool bNormalize = false;
		FVector3f OffsetAxis = FVector3f::ZeroVector;
		float OffsetRotation = 0.0f, Scale = 1.0f;
		virtual void Configure() override
		{
			ControlPoint = I(TEXT("control point"), 0);
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 0), EAttr::Xyz);
			bNormalize = B(TEXT("normalize"), false);
			OffsetAxis = V(TEXT("offset axis"), FVector3f::ZeroVector);
			OffsetRotation = F(TEXT("offset rotation"), 0.0f);
			Scale = F(TEXT("scale factor"), 1.0f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const FVector3f CPPos = Ctx.CP(ControlPoint).Position;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				P.SetVector(Output, i, DirectionToCP(CPPos, P.GetVector(EAttr::Xyz, i), bNormalize, OffsetAxis, OffsetRotation, Scale));
			}
		}
	};

	/** Normal Lock to Control Point. */
	class FNormalLockToControlPoint : public FSourceParticleOp
	{
		int32 ControlPoint = 0;
		virtual void Configure() override { ControlPoint = I(TEXT("control_point_number"), 0); }
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const FVector3f Forward = Ctx.CP(ControlPoint).Forward.GetSafeNormal();
			for (int32 i = 0; i < P.Num(); ++i)
			{
				P.SetVector(EAttr::Normal, i, Forward);
			}
		}
	};

	/** Inherit Attribute From Parent Particle. */
	class FInheritAttributeFromParentParticle : public FSourceParticleOp
	{
		EAttr Field = EAttr::Radius;
		float Scale = 1.0f;
		bool bRandom = false;
		int32 Increment = 1;
		virtual void Configure() override
		{
			Field = SourceParticleAttr::FromField(I(TEXT("Inherited Field"), 3), EAttr::Radius);
			Scale = F(TEXT("Scale"), 1.0f);
			bRandom = B(TEXT("Random Parent Particle Distribution"), false);
			Increment = FMath::Max(1, I(TEXT("Particle Increment Amount"), 1));
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const FSourceParticleCollection* Parent = Ctx.ParentParticles;
			if (!Parent || Parent->Num() == 0)
			{
				return;
			}
			const int32 Comps = SourceParticleAttr::Components(Field);
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const int32 Source = bRandom
					? FMath::Min((int32)(StableRand(P, i, 0) * Parent->Num()), Parent->Num() - 1)
					: (i * Increment) % Parent->Num();
				for (int32 c = 0; c < Comps; ++c)
				{
					P.SetFloat(Field, i, Parent->GetFloat(Field, Source, c) * Scale, c);
				}
			}
		}
	};

	/** Set Control Point To Particles' Center. */
	class FSetControlPointToParticlesCenter : public FSourceParticleOp
	{
		int32 ControlPoint = 1;
		FVector3f Offset = FVector3f::ZeroVector;
		virtual void Configure() override
		{
			ControlPoint = I(TEXT("Control Point Number to Set"), 1);
			Offset = V(TEXT("Center Offset"), FVector3f::ZeroVector);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (P.Num() == 0)
			{
				return;
			}
			FVector3f Sum = FVector3f::ZeroVector;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				Sum += P.GetVector(EAttr::Xyz, i);
			}
			Ctx.CP(ControlPoint).Position = Sum / (float)P.Num() + Offset;
		}
	};

	/** Set control points from particle positions. */
	class FSetControlPointsFromParticlePositions : public FSourceParticleOp
	{
		int32 FirstCP = 0, NumToSet = 1, FirstParticle = 0;
		virtual void Configure() override
		{
			FirstCP = I(TEXT("First control point to set"), 0);
			NumToSet = I(TEXT("# of control points to set"), 1);
			FirstParticle = I(TEXT("first particle to copy"), 0);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			for (int32 i = 0; i < NumToSet; ++i)
			{
				const int32 Particle = FirstParticle + i;
				const int32 CP = FirstCP + i;
				if (Particle < 0 || Particle >= P.Num() || CP < 0 || CP >= FSourceParticleContext::MaxControlPoints)
				{
					continue;
				}
				Ctx.ControlPoints[CP].Position = P.GetVector(EAttr::Xyz, Particle);
			}
		}
	};

	/** Set Control Point Positions: up to four control points put at fixed spots. */
	class FSetControlPointPositions : public FSourceParticleOp
	{
		int32 Numbers[4] = { 1, 2, 3, 4 };
		FVector3f Locations[4] = { FVector3f(128, 0, 0), FVector3f(0, 128, 0), FVector3f(-128, 0, 0), FVector3f(0, -128, 0) };
		bool bWorldSpace = false;
		int32 OffsetFromCP = 0;
		virtual void Configure() override
		{
			Numbers[0] = I(TEXT("First Control Point Number"), 1);
			Numbers[1] = I(TEXT("Second Control Point Number"), 2);
			Numbers[2] = I(TEXT("Third Control Point Number"), 3);
			Numbers[3] = I(TEXT("Fourth Control Point Number"), 4);
			Locations[0] = V(TEXT("First Control Point Location"), FVector3f(128, 0, 0));
			Locations[1] = V(TEXT("Second Control Point Location"), FVector3f(0, 128, 0));
			Locations[2] = V(TEXT("Third Control Point Location"), FVector3f(-128, 0, 0));
			Locations[3] = V(TEXT("Fourth Control Point Location"), FVector3f(0, -128, 0));
			bWorldSpace = B(TEXT("Set positions in world space"), false);
			OffsetFromCP = I(TEXT("Control Point to offset positions from"), 0);
		}
		virtual bool RunBeforeEmitters() const override { return true; }
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const FSourceParticleControlPoint Origin = bWorldSpace ? FSourceParticleControlPoint() : Ctx.CP(OffsetFromCP);
			for (int32 i = 0; i < 4; ++i)
			{
				if (Numbers[i] < 0 || Numbers[i] >= FSourceParticleContext::MaxControlPoints)
				{
					continue;
				}
				Ctx.ControlPoints[Numbers[i]].Position = bWorldSpace ? Locations[i] : Origin.Position + Origin.TransformLocal(Locations[i]);
			}
		}
	};

	/** Set Control Point To Player: a control point follows the local player. */
	class FSetControlPointToPlayer : public FSourceParticleOp
	{
		int32 ControlPoint = 1;
		FVector3f Offset = FVector3f::ZeroVector;
		bool bEyeOrientation = false;
		virtual void Configure() override
		{
			ControlPoint = I(TEXT("Control Point Number"), 1);
			Offset = V(TEXT("Control Point Offset"), FVector3f::ZeroVector);
			bEyeOrientation = B(TEXT("Use Eye Orientation"), false);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (!Ctx.bHavePlayer)
			{
				return;
			}
			FSourceParticleControlPoint& CP = Ctx.CP(ControlPoint);
			if (bEyeOrientation)
			{
				CP.SetOrientationFromAngles(Ctx.PlayerEyeAngles);
				CP.Position = Ctx.PlayerPosition + CP.TransformLocal(Offset);
			}
			else
			{
				CP.Position = Ctx.PlayerPosition + Offset;
			}
		}
	};

	/** Set CP Orientation to CP Direction. */
	class FSetCPOrientationToCPDirection : public FSourceParticleOp
	{
		int32 InputCP = 0, OutputCP = 0;
		virtual void Configure() override
		{
			InputCP = I(TEXT("input control point"), 0);
			OutputCP = I(TEXT("output control point"), 0);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			FSourceParticleControlPoint& Out = Ctx.CP(OutputCP);
			FVector3f Forward = Ctx.CP(InputCP).Position - Out.Position;
			if (Forward.SizeSquared() < 1e-8f)
			{
				return;
			}
			Forward.Normalize();
			FVector3f Right, Up;
			VectorVectors(Forward, Right, Up);
			Out.Forward = Forward;
			Out.Right = Right;
			Out.Up = Up;
		}
	};

	/** Set Control Point Rotation: a control point's basis spun about an axis. */
	class FSetControlPointRotation : public FSourceParticleOp
	{
		FVector3f Axis = FVector3f(0, 0, 1);
		float Rate = 180.0f;
		int32 ControlPoint = 0, LocalSpaceCP = -1;
		virtual void Configure() override
		{
			Axis = V(TEXT("Rotation Axis"), FVector3f(0, 0, 1));
			Rate = F(TEXT("Rotation Rate"), 180.0f);
			ControlPoint = I(TEXT("Control Point"), 0);
			LocalSpaceCP = I(TEXT("Local Space Control Point"), -1);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (Ctx.DeltaTime <= 0.0f || Rate == 0.0f)
			{
				return;
			}
			FVector3f A = Axis;
			if (LocalSpaceCP >= 0 && LocalSpaceCP < FSourceParticleContext::MaxControlPoints)
			{
				A = Ctx.ControlPoints[LocalSpaceCP].TransformLocal(A);
			}
			if (A.SizeSquared() < 1e-8f)
			{
				return;
			}
			A.Normalize();
			const float Angle = Rate * DegToRad * Ctx.DeltaTime * Strength;
			FSourceParticleControlPoint& CP = Ctx.CP(ControlPoint);
			CP.Forward = RotateAboutAxis(CP.Forward, A, Angle);
			CP.Right = RotateAboutAxis(CP.Right, A, Angle);
			CP.Up = RotateAboutAxis(CP.Up, A, Angle);
		}
	};

	/** Remap CP Speed to CP. */
	class FRemapCPSpeedToCP : public FSourceParticleOp
	{
		int32 InputCP = 0, OutputCP = -1, OutputField = 0;
		float InMin = 0.0f, InMax = 1.0f, OutMin = 0.0f, OutMax = 1.0f;
		virtual void Configure() override
		{
			InputCP = I(TEXT("input control point"), 0);
			InMin = F(TEXT("input minimum"), 0.0f);
			InMax = F(TEXT("input maximum"), 1.0f);
			OutputCP = I(TEXT("output control point"), -1);
			OutputField = I(TEXT("Output field 0-2 X/Y/Z"), 0);
			OutMin = F(TEXT("output minimum"), 0.0f);
			OutMax = F(TEXT("output maximum"), 1.0f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const float Speed = Ctx.CP(InputCP).Velocity(Ctx.DeltaTime).Size();
			const float Result = Lerp(OutMin, OutMax, RemapClamped(Speed, InMin, InMax));
			SetCPField(Ctx, OutputCP < 0 ? InputCP : OutputCP, OutputField, Result);
		}
	};

	/** Remap Average Scalar Value to CP. */
	class FRemapAverageScalarValueToCP : public FSourceParticleOp
	{
		EAttr Field = EAttr::Radius;
		float InMin = 0.0f, InMax = 1.0f, OutMin = 0.0f, OutMax = 1.0f;
		int32 OutputCP = 1;
		virtual void Configure() override
		{
			Field = SourceParticleAttr::FromField(I(TEXT("Scalar field"), 3), EAttr::Radius);
			InMin = F(TEXT("input volume minimum"), 0.0f);
			InMax = F(TEXT("input volume maximum"), 1.0f);
			OutputCP = I(TEXT("output control point"), 1);
			OutMin = F(TEXT("output minimum"), 0.0f);
			OutMax = F(TEXT("output maximum"), 1.0f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (P.Num() == 0)
			{
				return;
			}
			float Sum = 0.0f;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				Sum += P.GetFloat(Field, i);
			}
			SetCPField(Ctx, OutputCP, 0, Lerp(OutMin, OutMax, RemapClamped(Sum / P.Num(), InMin, InMax)));
		}
	};

	/** Remap Distance Between Two Control Points to CP. */
	class FRemapDistanceBetweenTwoControlPointsToCP : public FSourceParticleOp
	{
		float DistMin = 0.0f, DistMax = 128.0f, OutMin = 0.0f, OutMax = 1.0f;
		int32 OutputCP = 2, OutputField = 0, StartCP = 0, EndCP = 1;
		virtual void Configure() override
		{
			DistMin = F(TEXT("distance minimum"), 0.0f);
			DistMax = F(TEXT("distance maximum"), 128.0f);
			OutputCP = I(TEXT("output control point"), 2);
			OutputField = I(TEXT("output control point field"), 0);
			OutMin = F(TEXT("output minimum"), 0.0f);
			OutMax = F(TEXT("output maximum"), 1.0f);
			StartCP = I(TEXT("starting control point"), 0);
			EndCP = I(TEXT("ending control point"), 1);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const float Distance = (Ctx.CP(EndCP).Position - Ctx.CP(StartCP).Position).Size();
			SetCPField(Ctx, OutputCP, OutputField, Lerp(OutMin, OutMax, RemapClamped(Distance, DistMin, DistMax)));
		}
	};

	/** Remap Particle BBox Volume to CP. */
	class FRemapParticleBBoxVolumeToCP : public FSourceParticleOp
	{
		float InMin = 0.0f, InMax = 128.0f, OutMin = 0.0f, OutMax = 1.0f;
		int32 OutputCP = -1;
		virtual void Configure() override
		{
			InMin = F(TEXT("input volume minimum in cubic units"), 0.0f);
			InMax = F(TEXT("input volume maximum in cubic units"), 128.0f);
			OutputCP = I(TEXT("output control point"), -1);
			OutMin = F(TEXT("output minimum"), 0.0f);
			OutMax = F(TEXT("output maximum"), 1.0f);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (P.Num() == 0)
			{
				return;
			}
			FVector3f Min = P.GetVector(EAttr::Xyz, 0), Max = Min;
			for (int32 i = 1; i < P.Num(); ++i)
			{
				const FVector3f Pos = P.GetVector(EAttr::Xyz, i);
				Min = FVector3f::Min(Min, Pos);
				Max = FVector3f::Max(Max, Pos);
			}
			const FVector3f Size = Max - Min;
			SetCPField(Ctx, OutputCP < 0 ? 0 : OutputCP, 0, Lerp(OutMin, OutMax, RemapClamped(Size.X * Size.Y * Size.Z, InMin, InMax)));
		}
	};

	/** Set CP Offset to CP Percentage Between Two Control Points. */
	class FSetCPOffsetToCPPercentageBetweenTwoControlPoints : public FSourceParticleOp
	{
		float PctMin = 0.0f, PctMax = 1.0f, PctBias = 0.5f;
		int32 StartCP = 0, EndCP = 1, OffsetCP = 2, InputCP = 3, OutputCP = 4;
		FVector3f OffsetAmount = FVector3f::ZeroVector;
		bool bRadius = true, bScaleOffset = false;
		virtual void Configure() override
		{
			PctMin = F(TEXT("percentage minimum"), 0.0f);
			PctMax = F(TEXT("percentage maximum"), 1.0f);
			PctBias = F(TEXT("percentage bias"), 0.5f);
			StartCP = I(TEXT("starting control point"), 0);
			EndCP = I(TEXT("ending control point"), 1);
			OffsetCP = I(TEXT("offset control point"), 2);
			InputCP = I(TEXT("input control point"), 3);
			OutputCP = I(TEXT("output control point"), 4);
			OffsetAmount = V(TEXT("offset amount"), FVector3f::ZeroVector);
			bRadius = B(TEXT("treat distance between points as radius"), true);
			bScaleOffset = B(TEXT("treat offset as scale of total distance"), false);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			const FVector3f Start = Ctx.CP(StartCP).Position;
			const FVector3f Axis = Ctx.CP(EndCP).Position - Start;
			float Pct = RemapClamped(PercentageBetween(Ctx.CP(InputCP).Position, Start, Axis, bRadius), PctMin, PctMax);
			if (PctBias != 0.5f && PctBias > 0.0f)
			{
				Pct = Bias(Pct, PctBias);
			}
			FVector3f Offset = OffsetAmount * Pct;
			if (bScaleOffset)
			{
				Offset *= Axis.Size();
			}
			Ctx.CP(OutputCP).Position = Ctx.CP(OffsetCP).Position + Offset;
		}
	};

	/** Orients a child's control point along the particle's motion (the "set orientation" option). */
	static void OrientChildCP(const FSourceParticleCollection& P, FSourceParticleChildSim& Child, int32 CPIndex, int32 Particle)
	{
		FVector3f Fwd = P.GetVector(EAttr::Xyz, Particle) - P.GetVector(EAttr::PrevXyz, Particle);
		if (Fwd.SizeSquared() < 1e-10f)
		{
			return;
		}
		Fwd.Normalize();
		FVector3f Right, Up;
		VectorVectors(Fwd, Right, Up);
		FSourceParticleControlPoint& CP = Child.Simulator->GetContext().ControlPoints[CPIndex];
		CP.Forward = Fwd;
		CP.Right = Right;
		CP.Up = Up;
	}

	/** Set child control points from particle positions. */
	class FSetChildControlPointsFromParticlePositions : public FSourceParticleOp
	{
		int32 GroupId = 0, FirstCP = 0, NumToSet = 1, FirstParticle = 0;
		bool bSetOrientation = false;
		virtual void Configure() override
		{
			GroupId = I(TEXT("Group ID to affect"), 0);
			FirstCP = I(TEXT("First control point to set"), 0);
			NumToSet = I(TEXT("# of control points to set"), 1);
			FirstParticle = I(TEXT("first particle to copy"), 0);
			bSetOrientation = B(TEXT("set orientation"), false);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (!Ctx.Children || Ctx.Children->Num() == 0)
			{
				return;
			}
			const int32 First = FMath::Clamp(FirstCP, 0, FSourceParticleContext::MaxControlPoints - 1);
			int32 ToSet = FMath::Min(P.Num() - FirstParticle, NumToSet);
			ToSet = FMath::Min(ToSet, FSourceParticleContext::MaxControlPoints - First);
			if (ToSet <= 0)
			{
				return;
			}
			for (FSourceParticleChildSim& Child : *Ctx.Children)
			{
				if (Child.Simulator->GetDefinition().GroupId != GroupId)
				{
					continue;
				}
				for (int32 i = 0; i < ToSet; ++i)
				{
					const int32 Src = i + FirstParticle;
					const int32 Idx = i + First;
					Child.Simulator->GetContext().ControlPoints[Idx].Position = P.GetVector(EAttr::Xyz, Src);
					if (bSetOrientation)
					{
						OrientChildCP(P, Child, Idx, Src);
					}
					Child.OverriddenControlPoints.Add(Idx);
				}
			}
		}
	};

	/** Set per child control point from particle positions. */
	class FSetPerChildControlPointFromParticlePositions : public FSourceParticleOp
	{
		int32 GroupId = 0, ControlPoint = 0, NumChildren = 1, FirstParticle = 0;
		bool bSetOrientation = false;
		virtual void Configure() override
		{
			GroupId = I(TEXT("Group ID to affect"), 0);
			ControlPoint = I(TEXT("control point to set"), 0);
			NumChildren = I(TEXT("# of children to set"), 1);
			FirstParticle = I(TEXT("first particle to copy"), 0);
			bSetOrientation = B(TEXT("set orientation"), false);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (!Ctx.Children || Ctx.Children->Num() == 0)
			{
				return;
			}
			const int32 CP = FMath::Clamp(ControlPoint, 0, FSourceParticleContext::MaxControlPoints - 1);
			int32 ToSet = FMath::Min(NumChildren, FMath::Min(P.Num() - FirstParticle, Ctx.Children->Num()));
			int32 Current = FirstParticle;
			for (FSourceParticleChildSim& Child : *Ctx.Children)
			{
				if (ToSet <= 0)
				{
					break;
				}
				if (Child.Simulator->GetDefinition().GroupId != GroupId)
				{
					continue;
				}
				Child.Simulator->GetContext().ControlPoints[CP].Position = P.GetVector(EAttr::Xyz, Current);
				if (bSetOrientation)
				{
					OrientChildCP(P, Child, CP, Current);
				}
				Child.OverriddenControlPoints.Add(CP);
				--ToSet;
				++Current;
			}
		}
	};

	// ---- Effect control -----------------------------------------------------------------------------------------

	/** Stop Effect after Duration. */
	class FStopEffectAfterDuration : public FSourceParticleOp
	{
		float Duration = 1.0f;
		int32 ScaleCP = -1, ScaleField = 0;
		bool bDestroyAll = false, bPlayEndCap = true;
		virtual void Configure() override
		{
			Duration = F(TEXT("Duration at which to Stop"), 1.0f);
			ScaleCP = I(TEXT("Control Point to Scale Duration"), -1);
			ScaleField = I(TEXT("Control Point Field X/Y/Z"), 0);
			bDestroyAll = B(TEXT("Destroy All Particles Immediately"), false);
			bPlayEndCap = B(TEXT("Play End Cap Effect"), true);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			float D = Duration;
			if (ScaleCP >= 0 && ScaleCP < FSourceParticleContext::MaxControlPoints)
			{
				D *= Ctx.CPField(ScaleCP, ScaleField);
			}
			if (Ctx.Time < D)
			{
				return;
			}
			Ctx.bEmissionStopped = true;
			if (bPlayEndCap)
			{
				Ctx.bInEndCap = true;
			}
			if (bDestroyAll)
			{
				P.KillAll();
			}
		}
	};

	/** Restart Effect after Duration. */
	class FRestartEffectAfterDuration : public FSourceParticleOp
	{
		float MinRestart = 0.0f, MaxRestart = 1.0f;
		int32 ScaleCP = -1, ScaleField = 0;
		float LastRestart = 0.0f, NextInterval = -1.0f;
		int32 DrawCounter = 0;
		virtual void Configure() override
		{
			MinRestart = F(TEXT("Minimum Restart Time"), 0.0f);
			MaxRestart = F(TEXT("Maximum Restart Time"), 1.0f);
			ScaleCP = I(TEXT("Control Point to Scale Duration"), -1);
			ScaleField = I(TEXT("Control Point Field X/Y/Z"), 0);
		}
		virtual void Reset() override
		{
			LastRestart = 0.0f;
			NextInterval = -1.0f;
			DrawCounter = 0;
		}
		float DrawInterval(const FSourceParticleContext& Ctx)
		{
			float Interval = FSourceParticleRandom::Range(InstanceSeed, DrawCounter++, MinRestart, MaxRestart);
			if (ScaleCP >= 0 && ScaleCP < FSourceParticleContext::MaxControlPoints)
			{
				Interval *= Ctx.CPField(ScaleCP, ScaleField);
			}
			return FMath::Max(1e-4f, Interval);
		}
		virtual void Operate(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength) override
		{
			if (NextInterval < 0.0f)
			{
				NextInterval = DrawInterval(Ctx);
			}
			if (Ctx.Time - LastRestart < NextInterval)
			{
				return;
			}
			P.KillAll();
			Ctx.bEmissionStopped = false;
			LastRestart = Ctx.Time;
			NextInterval = DrawInterval(Ctx);
		}
	};
}

void RegisterSourceParticleOperatorOps(FSourceParticleOpRegistry& R)
{
	LAMBDA_PARTICLE_OP(R, "operators", "Movement Basic", FMovementBasic);
	LAMBDA_PARTICLE_OP(R, "operators", "Movement Lock to Control Point", FMovementLockToControlPoint);
	LAMBDA_PARTICLE_OP(R, "operators", "Movement Max Velocity", FMovementMaxVelocity);
	LAMBDA_PARTICLE_OP(R, "operators", "Movement Dampen Relative to Control Point", FMovementDampenRelativeToControlPoint);
	LAMBDA_PARTICLE_OP(R, "operators", "Movement Rotate Particle Around Axis", FMovementRotateParticleAroundAxis);
	LAMBDA_PARTICLE_OP(R, "operators", "Movement Match Particle Velocities", FMovementMatchParticleVelocities);
	LAMBDA_PARTICLE_OP(R, "operators", "Movement Maintain Offset", FMovementMaintainOffset);
	LAMBDA_PARTICLE_OP(R, "operators", "Movement Maintain Position Along Path", FMovementMaintainPositionAlongPath);
	LAMBDA_PARTICLE_OP(R, "operators", "Movement Place On Ground", FMovementPlaceOnGround);
	LAMBDA_PARTICLE_OP(R, "operators", "Lifespan Decay", FLifespanDecay);
	LAMBDA_PARTICLE_OP(R, "operators", "Lifespan Minimum Velocity Decay", FLifespanMinimumVelocityDecay);
	LAMBDA_PARTICLE_OP(R, "operators", "Lifespan Minimum Alpha Decay", FLifespanMinimumAlphaDecay);
	LAMBDA_PARTICLE_OP(R, "operators", "Lifespan Minimum Radius Decay", FLifespanMinimumRadiusDecay);
	LAMBDA_PARTICLE_OP(R, "operators", "Lifespan Maintain Count Decay", FLifespanMaintainCountDecay);
	LAMBDA_PARTICLE_OP(R, "operators", "Cull Random", FCullRandom);
	LAMBDA_PARTICLE_OP(R, "operators", "Cull when crossing plane", FCullWhenCrossingPlane);
	LAMBDA_PARTICLE_OP(R, "operators", "Cull when crossing sphere", FCullWhenCrossingSphere);
	LAMBDA_PARTICLE_OP(R, "operators", "Alpha Fade and Decay", FAlphaFadeAndDecay);
	LAMBDA_PARTICLE_OP(R, "operators", "Alpha Fade and Decay for Tracers", FAlphaFadeAndDecay);
	LAMBDA_PARTICLE_OP(R, "operators", "Alpha Fade In Random", FAlphaFadeInRandom);
	LAMBDA_PARTICLE_OP(R, "operators", "Alpha Fade Out Random", FAlphaFadeOutRandom);
	LAMBDA_PARTICLE_OP(R, "operators", "Alpha Fade In Simple", FAlphaFadeInSimple);
	LAMBDA_PARTICLE_OP(R, "operators", "Alpha Fade Out Simple", FAlphaFadeOutSimple);
	LAMBDA_PARTICLE_OP(R, "operators", "Radius Scale", FRadiusScale);
	LAMBDA_PARTICLE_OP(R, "operators", "Color Fade", FColorFade);
	LAMBDA_PARTICLE_OP(R, "operators", "Rotation Spin Roll", FRotationSpinRoll);
	LAMBDA_PARTICLE_OP(R, "operators", "Rotation Spin Yaw", FRotationSpinYaw);
	LAMBDA_PARTICLE_OP(R, "operators", "Rotation Basic", FRotationBasic);
	LAMBDA_PARTICLE_OP(R, "operators", "Rotation Orient to 2D Direction", FRotationOrientTo2DDirection);
	LAMBDA_PARTICLE_OP(R, "operators", "Rotation Orient Relative to CP", FRotationOrientRelativeToCP);
	LAMBDA_PARTICLE_OP(R, "operators", "Oscillate Scalar", FOscillateScalar);
	LAMBDA_PARTICLE_OP(R, "operators", "Oscillate Vector", FOscillateVector);
	LAMBDA_PARTICLE_OP(R, "operators", "Oscillate Scalar Simple", FOscillateScalarSimple);
	LAMBDA_PARTICLE_OP(R, "operators", "Oscillate Vector Simple", FOscillateVectorSimple);
	LAMBDA_PARTICLE_OP(R, "operators", "Noise Scalar", FNoiseScalar);
	LAMBDA_PARTICLE_OP(R, "operators", "Noise Vector", FNoiseVector);
	LAMBDA_PARTICLE_OP(R, "operators", "Remap Scalar", FRemapScalar);
	LAMBDA_PARTICLE_OP(R, "operators", "Clamp Scalar", FClampScalar);
	LAMBDA_PARTICLE_OP(R, "operators", "Clamp Vector", FClampVector);
	LAMBDA_PARTICLE_OP(R, "operators", "Normalize Vector", FNormalizeVector);
	LAMBDA_PARTICLE_OP(R, "operators", "Lerp Initial Scalar", FLerpInitialScalar);
	LAMBDA_PARTICLE_OP(R, "operators", "Lerp Initial Vector", FLerpInitialVector);
	LAMBDA_PARTICLE_OP(R, "operators", "Lerp EndCap Scalar", FLerpEndCapScalar);
	LAMBDA_PARTICLE_OP(R, "operators", "Lerp EndCap Vector", FLerpEndCapVector);
	LAMBDA_PARTICLE_OP(R, "operators", "Ramp Scalar Linear Simple", FRampScalarLinearSimple);
	LAMBDA_PARTICLE_OP(R, "operators", "Ramp Scalar Spline Simple", FRampScalarLinearSimple);
	LAMBDA_PARTICLE_OP(R, "operators", "Ramp Scalar Linear Random", FRampScalarLinearRandom);
	LAMBDA_PARTICLE_OP(R, "operators", "Ramp Scalar Spline Random", FRampScalarLinearRandom);
	LAMBDA_PARTICLE_OP(R, "operators", "Remap Distance to Control Point to Scalar", FRemapDistanceToControlPointToScalar);
	LAMBDA_PARTICLE_OP(R, "operators", "Remap Control Point to Scalar", FRemapControlPointToScalarOp);
	LAMBDA_PARTICLE_OP(R, "operators", "Remap Control Point to Vector", FRemapControlPointToVectorOp);
	LAMBDA_PARTICLE_OP(R, "operators", "Remap Velocity to Vector", FRemapVelocityToVector);
	LAMBDA_PARTICLE_OP(R, "operators", "Remap Speed to Scalar", FRemapSpeedToScalarOp);
	LAMBDA_PARTICLE_OP(R, "operators", "Remap Distance Between Two Control Points to Scalar", FRemapDistanceBetweenTwoControlPointsToScalar);
	LAMBDA_PARTICLE_OP(R, "operators", "Remap Percentage Between Two Control Points to Scalar", FRemapPercentageBetweenTwoControlPointsToScalar);
	LAMBDA_PARTICLE_OP(R, "operators", "Remap Percentage Between Two Control Points to Vector", FRemapPercentageBetweenTwoControlPointsToVector);
	LAMBDA_PARTICLE_OP(R, "operators", "Remap Dot Product to Scalar", FRemapDotProductToScalar);
	LAMBDA_PARTICLE_OP(R, "operators", "Rotate Vector Random", FRotateVectorRandom);
	LAMBDA_PARTICLE_OP(R, "operators", "Remap Control Point Direction to Vector", FRemapControlPointDirectionToVector);
	LAMBDA_PARTICLE_OP(R, "operators", "Remap CP Velocity to Vector", FRemapCPVelocityToVector);
	LAMBDA_PARTICLE_OP(R, "operators", "Remap Direction to CP to Vector", FRemapDirectionToCPToVector);
	LAMBDA_PARTICLE_OP(R, "operators", "Normal Lock to Control Point", FNormalLockToControlPoint);
	LAMBDA_PARTICLE_OP(R, "operators", "Inherit Attribute From Parent Particle", FInheritAttributeFromParentParticle);
	LAMBDA_PARTICLE_OP(R, "operators", "Set Control Point To Particles' Center", FSetControlPointToParticlesCenter);
	LAMBDA_PARTICLE_OP(R, "operators", "Set control points from particle positions", FSetControlPointsFromParticlePositions);
	LAMBDA_PARTICLE_OP(R, "operators", "Set Control Point Positions", FSetControlPointPositions);
	LAMBDA_PARTICLE_OP(R, "operators", "Set Control Point To Player", FSetControlPointToPlayer);
	LAMBDA_PARTICLE_OP(R, "operators", "Set CP Orientation to CP Direction", FSetCPOrientationToCPDirection);
	LAMBDA_PARTICLE_OP(R, "operators", "Set Control Point Rotation", FSetControlPointRotation);
	LAMBDA_PARTICLE_OP(R, "operators", "Remap CP Speed to CP", FRemapCPSpeedToCP);
	LAMBDA_PARTICLE_OP(R, "operators", "Remap Average Scalar Value to CP", FRemapAverageScalarValueToCP);
	LAMBDA_PARTICLE_OP(R, "operators", "Remap Distance Between Two Control Points to CP", FRemapDistanceBetweenTwoControlPointsToCP);
	LAMBDA_PARTICLE_OP(R, "operators", "Remap Particle BBox Volume to CP", FRemapParticleBBoxVolumeToCP);
	LAMBDA_PARTICLE_OP(R, "operators", "Set CP Offset to CP Percentage Between Two Control Points", FSetCPOffsetToCPPercentageBetweenTwoControlPoints);
	LAMBDA_PARTICLE_OP(R, "operators", "Set child control points from particle positions", FSetChildControlPointsFromParticlePositions);
	LAMBDA_PARTICLE_OP(R, "operators", "Set per child control point from particle positions", FSetPerChildControlPointFromParticlePositions);
	LAMBDA_PARTICLE_OP(R, "operators", "Stop Effect after Duration", FStopEffectAfterDuration);
	LAMBDA_PARTICLE_OP(R, "operators", "Restart Effect after Duration", FRestartEffectAfterDuration);
}
