#include "Particles/SourceParticleOpsInternal.h"

// The initializers (particles/builtin_initializers.cpp): run once over each newly created range, in file order.
// Per-particle randoms draw from the table with sample = PARTICLE_ID + the running operator offset, and a salt
// decorrelates several draws inside one initializer.

using namespace SourceParticleOpHelpers;
using EAttr = ESourceParticleAttr;

namespace
{
	class FInitializerOp : public FSourceParticleOp
	{
	protected:
		float Rand(const FSourceParticleCollection& P, const FSourceParticleContext& Ctx, int32 Particle, int32 Salt) const
		{
			return FrameRand(P, Ctx, Particle, Salt);
		}
		float RandRange(const FSourceParticleCollection& P, const FSourceParticleContext& Ctx, int32 Particle, int32 Salt, float Min, float Max) const
		{
			return FrameRandRange(P, Ctx, Particle, Salt, Min, Max);
		}
		float RandRangeExp(const FSourceParticleCollection& P, const FSourceParticleContext& Ctx, int32 Particle, int32 Salt, float Min, float Max, float Exp) const
		{
			return FrameRandRangeExp(P, Ctx, Particle, Salt, Min, Max, Exp);
		}
		FVector3f RandVector(const FSourceParticleCollection& P, const FSourceParticleContext& Ctx, int32 Particle, int32 Salt, const FVector3f& Min, const FVector3f& Max) const
		{
			return FrameRandVector(P, Ctx, Particle, Salt, Min, Max);
		}
		FVector3f RandUnit(const FSourceParticleCollection& P, const FSourceParticleContext& Ctx, int32 Particle, int32 Salt) const
		{
			return FrameRandUnit(P, Ctx, Particle, Salt);
		}
		static void SetPosition(FSourceParticleCollection& P, int32 Particle, const FVector3f& Position)
		{
			P.SetVector(EAttr::Xyz, Particle, Position);
			P.SetVector(EAttr::PrevXyz, Particle, Position);
		}
		static void OffsetPosition(FSourceParticleCollection& P, int32 Particle, const FVector3f& Offset)
		{
			P.SetVector(EAttr::Xyz, Particle, P.GetVector(EAttr::Xyz, Particle) + Offset);
			P.SetVector(EAttr::PrevXyz, Particle, P.GetVector(EAttr::PrevXyz, Particle) + Offset);
		}
		/** Noise in [-1,1] folded to a [0,1] interpolant, with the abs / inverted-abs options. */
		static float Fold(float Noise, bool bAbs, bool bInvert)
		{
			if (bAbs)
			{
				const float Folded = FMath::Abs(Noise);
				return bInvert ? 1.0f - Folded : Folded;
			}
			return Noise * 0.5f + 0.5f;
		}
	};

	// ---- Position -----------------------------------------------------------------------------------------------

	/** Position Within Sphere Random: the usual position-and-velocity initializer. */
	class FPositionWithinSphereRandom : public FInitializerOp
	{
		float DistanceMin = 0.0f, DistanceMax = 0.0f, SpeedMin = 0.0f, SpeedMax = 0.0f, SpeedExp = 1.0f;
		FVector3f DistanceBias = FVector3f(1, 1, 1), DistanceBiasAbs = FVector3f::ZeroVector;
		FVector3f LocalSpeedMin = FVector3f::ZeroVector, LocalSpeedMax = FVector3f::ZeroVector;
		bool bBiasInLocalSystem = false;
		int32 ControlPoint = 0;

		virtual void Configure() override
		{
			DistanceMin = F(TEXT("distance_min"), 0.0f);
			DistanceMax = F(TEXT("distance_max"), 0.0f);
			DistanceBias = V(TEXT("distance_bias"), FVector3f(1, 1, 1));
			DistanceBiasAbs = V(TEXT("distance_bias_absolute_value"), FVector3f::ZeroVector);
			bBiasInLocalSystem = B(TEXT("bias in local system"), false);
			ControlPoint = I(TEXT("control_point_number"), 0);
			SpeedMin = F(TEXT("speed_min"), 0.0f);
			SpeedMax = F(TEXT("speed_max"), 0.0f);
			SpeedExp = F(TEXT("speed_random_exponent"), 1.0f);
			LocalSpeedMin = V(TEXT("speed_in_local_coordinate_system_min"), FVector3f::ZeroVector);
			LocalSpeedMax = V(TEXT("speed_in_local_coordinate_system_max"), FVector3f::ZeroVector);
		}

		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FSourceParticleControlPoint& CP = Ctx.CP(ControlPoint);
			for (int32 i = Start; i < Start + Count; ++i)
			{
				FVector3f Direction = RandUnit(P, Ctx, i, 0);
				// The per-axis bias, folded to one side where the absolute flag is set (a hemisphere), then renormalised.
				if (DistanceBias != FVector3f(1, 1, 1) || DistanceBiasAbs != FVector3f::ZeroVector)
				{
					Direction = FVector3f(Direction.X * DistanceBias.X, Direction.Y * DistanceBias.Y, Direction.Z * DistanceBias.Z);
					if (DistanceBiasAbs.X != 0.0f) { Direction.X = FMath::Abs(Direction.X); }
					if (DistanceBiasAbs.Y != 0.0f) { Direction.Y = FMath::Abs(Direction.Y); }
					if (DistanceBiasAbs.Z != 0.0f) { Direction.Z = FMath::Abs(Direction.Z); }
					if (bBiasInLocalSystem)
					{
						Direction = CP.TransformLocal(Direction);
					}
					if (Direction.SizeSquared() > 1e-8f)
					{
						Direction.Normalize();
					}
				}
				const float Distance = RandRange(P, Ctx, i, 3, DistanceMin, DistanceMax);
				SetPosition(P, i, CP.Position + Direction * Distance);

				const float Speed = RandRangeExp(P, Ctx, i, 4, SpeedMin, SpeedMax, SpeedExp);
				FVector3f Velocity = Direction * Speed;
				const FVector3f Local = RandVector(P, Ctx, i, 5, LocalSpeedMin, LocalSpeedMax);
				if (!Local.IsZero())
				{
					Velocity += CP.TransformLocal(Local);
				}
				if (!Velocity.IsZero())
				{
					AddVelocity(P, Ctx, i, Velocity);
				}
			}
		}
	};

	/** Position Within Box Random: uniform inside a box around the control point. */
	class FPositionWithinBoxRandom : public FInitializerOp
	{
		FVector3f Min = FVector3f::ZeroVector, Max = FVector3f::ZeroVector;
		int32 ControlPoint = 0;
		bool bLocal = false;

		virtual void Configure() override
		{
			Min = V(TEXT("min"), FVector3f::ZeroVector);
			Max = V(TEXT("max"), FVector3f::ZeroVector);
			ControlPoint = I(TEXT("control point number"), 0);
			bLocal = B(TEXT("use local space"), false);
		}

		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FSourceParticleControlPoint& CP = Ctx.CP(ControlPoint);
			for (int32 i = Start; i < Start + Count; ++i)
			{
				FVector3f Offset = RandVector(P, Ctx, i, 0, Min, Max);
				if (bLocal)
				{
					Offset = CP.TransformLocal(Offset);
				}
				SetPosition(P, i, CP.Position + Offset);
			}
		}
	};

	/** Position Modify Offset Random: a random offset that adds no velocity. */
	class FPositionModifyOffsetRandom : public FInitializerOp
	{
		FVector3f OffsetMin = FVector3f::ZeroVector, OffsetMax = FVector3f::ZeroVector;
		int32 ControlPoint = 0;
		bool bLocal = false, bProportionalToRadius = false;

		virtual void Configure() override
		{
			ControlPoint = I(TEXT("control_point_number"), 0);
			OffsetMin = V(TEXT("offset min"), FVector3f::ZeroVector);
			OffsetMax = V(TEXT("offset max"), FVector3f::ZeroVector);
			bLocal = B(TEXT("offset in local space 0/1"), false);
			bProportionalToRadius = B(TEXT("offset proportional to radius 0/1"), false);
		}

		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FSourceParticleControlPoint& CP = Ctx.CP(ControlPoint);
			for (int32 i = Start; i < Start + Count; ++i)
			{
				FVector3f Offset = RandVector(P, Ctx, i, 0, OffsetMin, OffsetMax);
				if (bLocal)
				{
					Offset = CP.TransformLocal(Offset);
				}
				if (bProportionalToRadius)
				{
					Offset *= P.GetFloat(EAttr::Radius, i);
				}
				OffsetPosition(P, i, Offset);
			}
		}
	};

	/**
	 * Position Modify Warp Random (C_INIT_PositionWarp): scales each particle's offset from the control point
	 * per axis by a warp between min and max - a random one, or one that runs from min to max over the warp
	 * transition time (reversed if asked) so a burst grows or shrinks as it goes.
	 */
	class FPositionModifyWarpRandom : public FInitializerOp
	{
		FVector3f WarpMin = FVector3f(1, 1, 1), WarpMax = FVector3f(1, 1, 1);
		float WarpTime = 0.0f, WarpStartTime = 0.0f;
		bool bReverse = false, bUseCount = false;
		int32 ControlPoint = 0;

		virtual void Configure() override
		{
			ControlPoint = I(TEXT("control point number"), 0);
			WarpMin = V(TEXT("warp min"), FVector3f(1, 1, 1));
			WarpMax = V(TEXT("warp max"), FVector3f(1, 1, 1));
			WarpTime = F(TEXT("warp transition time (treats min/max as start/end sizes)"), 0.0f);
			WarpStartTime = F(TEXT("warp transition start time"), 0.0f);
			bReverse = B(TEXT("reverse warp (0/1)"), false);
			bUseCount = B(TEXT("use particle count instead of time"), false);
		}

		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FSourceParticleControlPoint& CP = Ctx.CP(ControlPoint);
			for (int32 i = Start; i < Start + Count; ++i)
			{
				FVector3f Warp;
				if (WarpTime > 0.0f)
				{
					const float Param = bUseCount ? (float)P.IdOf(i) : Ctx.Time;
					float T = Clamp01((Param - WarpStartTime) / WarpTime);
					if (bReverse)
					{
						T = 1.0f - T;
					}
					Warp = FMath::Lerp(WarpMin, WarpMax, T);
				}
				else
				{
					Warp = RandVector(P, Ctx, i, 0, WarpMin, WarpMax);
				}
				const FVector3f Offset = P.GetVector(EAttr::Xyz, i) - CP.Position;
				const FVector3f Warped(Offset.X * Warp.X, Offset.Y * Warp.Y, Offset.Z * Warp.Z);
				OffsetPosition(P, i, Warped - Offset);
			}
		}
	};

	/** Position Modify Place On Ground: drops the particle onto whatever is below it (a trace into the world). */
	class FPositionModifyPlaceOnGround : public FInitializerOp
	{
		float Offset = 0.0f, MaxTraceLength = 128.0f;
		bool bKillOnNoCollision = false, bSetNormal = false;

		virtual void Configure() override
		{
			Offset = F(TEXT("offset"), 0.0f);
			bKillOnNoCollision = B(TEXT("kill on no collision"), false);
			bSetNormal = B(TEXT("set normal"), false);
			MaxTraceLength = F(TEXT("max trace length"), 128.0f);
		}

		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			if (!Ctx.TraceLine)
			{
				return;
			}
			for (int32 i = Start; i < Start + Count; ++i)
			{
				const FVector3f Pos = P.GetVector(EAttr::Xyz, i);
				FSourceParticleTraceHit Hit;
				if (Ctx.TraceLine(Pos, Pos - FVector3f(0, 0, MaxTraceLength), Hit) && Hit.bHit)
				{
					SetPosition(P, i, Hit.Position + FVector3f(0, 0, Offset));
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

	/** Position Along Ring: on a ring (of some thickness) around the control point, launched outward. */
	class FPositionAlongRing : public FInitializerOp
	{
		int32 ControlPoint = 0;
		float InitialRadius = 0.0f, Thickness = 0.0f, MinSpeed = 0.0f, MaxSpeed = 0.0f, Yaw = 0.0f, Roll = 0.0f, Pitch = 0.0f;
		bool bEven = false, bXYOnly = true;
		float EvenCount = -1.0f;

		virtual void Configure() override
		{
			ControlPoint = I(TEXT("control point number"), 0);
			InitialRadius = F(TEXT("initial radius"), 0.0f);
			Thickness = F(TEXT("thickness"), 0.0f);
			MinSpeed = F(TEXT("min initial speed"), 0.0f);
			MaxSpeed = F(TEXT("max initial speed"), 0.0f);
			Yaw = F(TEXT("yaw"), 0.0f);
			Roll = F(TEXT("roll"), 0.0f);
			Pitch = F(TEXT("pitch"), 0.0f);
			bEven = B(TEXT("even distribution"), false);
			EvenCount = F(TEXT("even distribution count"), -1.0f);
			bXYOnly = B(TEXT("XY velocity only"), true);
		}

		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FSourceParticleControlPoint& CP = Ctx.CP(ControlPoint);
			FSourceParticleControlPoint Basis;
			Basis.SetOrientationFromAngles(FVector3f(Pitch, Yaw, Roll));
			for (int32 i = Start; i < Start + Count; ++i)
			{
				float Angle;
				if (bEven)
				{
					const int32 Slots = EvenCount > 0.0f ? (int32)EvenCount : FMath::Max(1, Count);
					Angle = (P.IdOf(i) % Slots) * (2.0f * PI) / Slots;
				}
				else
				{
					Angle = Rand(P, Ctx, i, 0) * 2.0f * PI;
				}
				float Radius = InitialRadius;
				if (Thickness > 0.0f)
				{
					Radius += (Rand(P, Ctx, i, 3) * 2.0f - 1.0f) * Thickness;
				}
				// The ring lies in the plane the pitch/yaw/roll orient; its own X/Y are forward and right.
				const FVector3f Radial = Basis.Forward * FMath::Cos(Angle) + Basis.Right * FMath::Sin(Angle);
				SetPosition(P, i, CP.Position + Radial * Radius);

				const float Speed = RandRange(P, Ctx, i, 6, MinSpeed, MaxSpeed);
				if (Speed != 0.0f)
				{
					FVector3f Velocity = Radial * Speed;
					if (bXYOnly)
					{
						Velocity.Z = 0.0f;
					}
					AddVelocity(P, Ctx, i, Velocity);
				}
			}
		}
	};

	/** Position From Parent Particles: each particle starts where one of the parent's particles is. */
	class FPositionFromParentParticles : public FInitializerOp
	{
		float InheritedVelocityScale = 0.0f;
		bool bRandom = false;
		int32 Increment = 1;
		int32 Cursor = 0;

		virtual void Configure() override
		{
			InheritedVelocityScale = F(TEXT("Inherited Velocity Scale"), 0.0f);
			bRandom = B(TEXT("Random Parent Particle Distribution"), false);
			Increment = I(TEXT("Particle Increment Amount"), 1);
		}
		virtual void Reset() override { Cursor = 0; }

		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FSourceParticleCollection* Parent = Ctx.ParentParticles;
			if (!Parent || Parent->Num() == 0)
			{
				return;
			}
			for (int32 i = Start; i < Start + Count; ++i)
			{
				int32 Source;
				if (bRandom)
				{
					Source = FMath::Min((int32)(Rand(P, Ctx, i, 0) * Parent->Num()), Parent->Num() - 1);
				}
				else
				{
					Source = Cursor % Parent->Num();
					Cursor += FMath::Max(1, Increment);
				}
				const FVector3f Position = Parent->GetVector(EAttr::Xyz, Source);
				SetPosition(P, i, Position);
				if (InheritedVelocityScale != 0.0f && Ctx.DeltaTime > 0.0f)
				{
					const FVector3f ParentVelocity = (Position - Parent->GetVector(EAttr::PrevXyz, Source)) / Ctx.DeltaTime;
					AddVelocity(P, Ctx, i, ParentVelocity * InheritedVelocityScale);
				}
			}
		}
	};

	/** Position from Parent Cache: a parent particle's position plus a random local offset. */
	class FPositionFromParentCache : public FInitializerOp
	{
		FVector3f OffsetMin = FVector3f::ZeroVector, OffsetMax = FVector3f::ZeroVector;
		bool bSetNormal = false;
		int32 Cursor = 0;

		virtual void Configure() override
		{
			OffsetMin = V(TEXT("Local Offset Min"), FVector3f::ZeroVector);
			OffsetMax = V(TEXT("Local Offset Max"), FVector3f::ZeroVector);
			bSetNormal = B(TEXT("Set Normal"), false);
		}
		virtual void Reset() override { Cursor = 0; }

		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FSourceParticleCollection* Parent = Ctx.ParentParticles;
			if (!Parent || Parent->Num() == 0)
			{
				return;
			}
			for (int32 i = Start; i < Start + Count; ++i)
			{
				const int32 Source = Cursor++ % Parent->Num();
				const FVector3f Offset = RandVector(P, Ctx, i, 0, OffsetMin, OffsetMax);
				SetPosition(P, i, Parent->GetVector(EAttr::Xyz, Source) + Offset);
				if (bSetNormal && Offset.SizeSquared() > 1e-8f)
				{
					P.SetVector(EAttr::Normal, i, Offset.GetSafeNormal());
				}
			}
		}
	};

	/** The bezier path spawners share their parameters. */
	class FPathInitializer : public FInitializerOp
	{
	protected:
		float MaximumDistance = 0.0f, Bulge = 0.0f, MidPointPosition = 0.5f;
		int32 StartCP = 0, EndCP = 0, BulgeControl = 0;

		void ConfigurePath(int32 DefaultEndCP)
		{
			MaximumDistance = F(TEXT("maximum distance"), 0.0f);
			Bulge = F(TEXT("bulge"), 0.0f);
			StartCP = I(TEXT("start control point number"), 0);
			EndCP = I(TEXT("end control point number"), DefaultEndCP);
			BulgeControl = I(TEXT("bulge control 0=random 1=orientation of start pnt 2=orientation of end point"), 0);
			MidPointPosition = F(TEXT("mid point position"), 0.5f);
		}
		FVector3f PathPoint(const FSourceParticleCollection& P, const FSourceParticleContext& Ctx, int32 Particle, float T) const
		{
			FVector3f Pos = Bezier(Ctx, StartCP, EndCP, MidPointPosition, Bulge, BulgeControl, RandUnit(P, Ctx, Particle, 1), T);
			if (MaximumDistance > 0.0f)
			{
				Pos += RandUnit(P, Ctx, Particle, 4) * (Rand(P, Ctx, Particle, 7) * MaximumDistance);
			}
			return Pos;
		}
	};

	/** Position Along Path Random: a random point along the path, scattered within the maximum distance. */
	class FPositionAlongPathRandom : public FPathInitializer
	{
		virtual void Configure() override { ConfigurePath(0); }
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			for (int32 i = Start; i < Start + Count; ++i)
			{
				SetPosition(P, i, PathPoint(P, Ctx, i, Rand(P, Ctx, i, 0)));
			}
		}
	};

	/** Position Along Path Sequential: particle ids map to evenly spaced path parameters, looping or bouncing. */
	class FPositionAlongPathSequential : public FPathInitializer
	{
		float Span = 100.0f;
		bool bLoop = true;

		virtual void Configure() override
		{
			ConfigurePath(0);
			Span = F(TEXT("particles to map from start to end"), 100.0f);
			bLoop = B(TEXT("restart behavior (0 = bounce, 1 = loop )"), true);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const float Divisor = Span > 0.0f ? Span : 1.0f;
			for (int32 i = Start; i < Start + Count; ++i)
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
				SetPosition(P, i, PathPoint(P, Ctx, i, T));
			}
		}
	};

	/** Position In CP Hierarchy: the same path, with the scatter shaped by a per-axis bias. */
	class FPositionInCPHierarchy : public FPathInitializer
	{
		FVector3f DistanceBias = FVector3f(1, 1, 1);

		virtual void Configure() override
		{
			ConfigurePath(1);
			DistanceBias = V(TEXT("distance_bias"), FVector3f(1, 1, 1));
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			for (int32 i = Start; i < Start + Count; ++i)
			{
				FVector3f Pos = Bezier(Ctx, StartCP, EndCP, MidPointPosition, Bulge, BulgeControl, RandUnit(P, Ctx, i, 1), Rand(P, Ctx, i, 0));
				if (MaximumDistance > 0.0f)
				{
					FVector3f Scatter = RandUnit(P, Ctx, i, 4) * (Rand(P, Ctx, i, 7) * MaximumDistance);
					Scatter = FVector3f(Scatter.X * DistanceBias.X, Scatter.Y * DistanceBias.Y, Scatter.Z * DistanceBias.Z);
					Pos += Scatter;
				}
				SetPosition(P, i, Pos);
			}
		}
	};

	/** Move Particles Between 2 Control Points: starts at the first and heads for the second. */
	class FMoveParticlesBetween2ControlPoints : public FInitializerOp
	{
		float MinSpeed = 1.0f, MaxSpeed = 1.0f, StartOffset = 0.0f;
		int32 EndCP = 1;

		virtual void Configure() override
		{
			MinSpeed = F(TEXT("minimum speed"), 1.0f);
			MaxSpeed = F(TEXT("maximum speed"), 1.0f);
			StartOffset = F(TEXT("start offset"), 0.0f);
			EndCP = I(TEXT("end control point"), 1);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FVector3f From = Ctx.CP(0).Position;
			const FVector3f Axis = Ctx.CP(EndCP).Position - From;
			const float Length = Axis.Size();
			if (Length < 1e-6f)
			{
				return;
			}
			const FVector3f Unit = Axis / Length;
			for (int32 i = Start; i < Start + Count; ++i)
			{
				SetPosition(P, i, From + Unit * StartOffset);
				const float Fraction = RandRange(P, Ctx, i, 0, MinSpeed, MaxSpeed);
				if (Fraction != 0.0f)
				{
					AddVelocity(P, Ctx, i, Unit * (Length * Fraction));
				}
			}
		}
	};

	/** Position Along Epitrochoid: a spirograph curve in two chosen axes around the control point. */
	class FPositionAlongEpitrochoid : public FInitializerOp
	{
		int32 ControlPoint = 0, Dim1 = 0, Dim2 = 1;
		float Radius1 = 40.0f, Radius2 = 24.0f, PointOffset = 4.0f, Density = 10.0f;
		bool bUseCount = false, bLocal = false, bOffsetExisting = false;

		virtual void Configure() override
		{
			ControlPoint = I(TEXT("control point number"), 0);
			Dim1 = I(TEXT("first dimension 0-2 (-1 disables)"), 0);
			Dim2 = I(TEXT("second dimension 0-2 (-1 disables)"), 1);
			Radius1 = F(TEXT("radius 1"), 40.0f);
			Radius2 = F(TEXT("radius 2"), 24.0f);
			PointOffset = F(TEXT("point offset"), 4.0f);
			Density = F(TEXT("particle density"), 10.0f);
			bUseCount = B(TEXT("use particle count instead of creation time"), false);
			bLocal = B(TEXT("local space"), false);
			bOffsetExisting = B(TEXT("offset from existing position"), false);
		}
		static void Assign(FVector3f& Out, int32 Dim, float Value)
		{
			if (Dim == 0) { Out.X = Value; }
			else if (Dim == 1) { Out.Y = Value; }
			else if (Dim == 2) { Out.Z = Value; }
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FSourceParticleControlPoint& CP = Ctx.CP(ControlPoint);
			const float Dens = Density != 0.0f ? Density : 1.0f;
			const float K = Radius2 != 0.0f ? (Radius1 + Radius2) / Radius2 : 1.0f;
			for (int32 i = Start; i < Start + Count; ++i)
			{
				const float Param = bUseCount ? (float)P.IdOf(i) : Ctx.Time;
				const float Theta = Param / Dens;
				const float U = (Radius1 + Radius2) * FMath::Cos(Theta) - PointOffset * FMath::Cos(K * Theta);
				const float W = (Radius1 + Radius2) * FMath::Sin(Theta) - PointOffset * FMath::Sin(K * Theta);
				FVector3f Local = FVector3f::ZeroVector;
				Assign(Local, Dim1, U);
				Assign(Local, Dim2, W);
				const FVector3f World = bLocal ? CP.TransformLocal(Local) : Local;
				const FVector3f Base = bOffsetExisting ? P.GetVector(EAttr::Xyz, i) : CP.Position;
				SetPosition(P, i, Base + World);
			}
		}
	};

	/** Position From Chaotic Attractor: a Pickover attractor, iterated from a per-particle seed. */
	class FPositionFromChaoticAttractor : public FInitializerOp
	{
		float A = -0.9629629f, Bp = 2.791139f, Cp = 1.85185185f, D = 1.5f, Scale = 1.0f;
		int32 ControlPoint = 0;

		virtual void Configure() override
		{
			A = F(TEXT("Pickover A Parameter"), -0.9629629f);
			Bp = F(TEXT("Pickover B Parameter"), 2.791139f);
			Cp = F(TEXT("Pickover C Parameter"), 1.85185185f);
			D = F(TEXT("Pickover D Parameter"), 1.5f);
			ControlPoint = I(TEXT("Relative Control point number"), 0);
			Scale = F(TEXT("Scale"), 1.0f);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FSourceParticleControlPoint& CP = Ctx.CP(ControlPoint);
			for (int32 i = Start; i < Start + Count; ++i)
			{
				float X = Rand(P, Ctx, i, 0) * 2.0f - 1.0f, Y = Rand(P, Ctx, i, 1) * 2.0f - 1.0f, Z = Rand(P, Ctx, i, 2) * 2.0f - 1.0f;
				for (int32 Iter = 0; Iter < 40; ++Iter)
				{
					const float Xn = FMath::Sin(A * Y) - Z * FMath::Cos(Bp * X);
					const float Yn = Z * FMath::Sin(Cp * X) - FMath::Cos(D * Y);
					const float Zn = FMath::Sin(X);
					X = Xn; Y = Yn; Z = Zn;
				}
				SetPosition(P, i, CP.Position + FVector3f(X, Y, Z) * Scale);
			}
		}
	};

	// ---- Velocity -----------------------------------------------------------------------------------------------

	/** Velocity Random: a random direction at a random speed, plus a per-axis velocity in the control point's frame. */
	class FVelocityRandom : public FInitializerOp
	{
		int32 ControlPoint = 0;
		float SpeedMin = 0.0f, SpeedMax = 0.0f;
		FVector3f LocalMin = FVector3f::ZeroVector, LocalMax = FVector3f::ZeroVector;

		virtual void Configure() override
		{
			ControlPoint = I(TEXT("control_point_number"), 0);
			SpeedMin = F(TEXT("random_speed_min"), 0.0f);
			SpeedMax = F(TEXT("random_speed_max"), 0.0f);
			LocalMin = V(TEXT("speed_in_local_coordinate_system_min"), FVector3f::ZeroVector);
			LocalMax = V(TEXT("speed_in_local_coordinate_system_max"), FVector3f::ZeroVector);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FSourceParticleControlPoint& CP = Ctx.CP(ControlPoint);
			for (int32 i = Start; i < Start + Count; ++i)
			{
				FVector3f Velocity = FVector3f::ZeroVector;
				if (SpeedMax != 0.0f || SpeedMin != 0.0f)
				{
					Velocity += RandUnit(P, Ctx, i, 1) * RandRange(P, Ctx, i, 0, SpeedMin, SpeedMax);
				}
				const FVector3f Local = RandVector(P, Ctx, i, 4, LocalMin, LocalMax);
				if (!Local.IsZero())
				{
					Velocity += CP.TransformLocal(Local);
				}
				if (!Velocity.IsZero())
				{
					AddVelocity(P, Ctx, i, Velocity);
				}
			}
		}
	};

	/** Velocity Noise: an initial velocity read off a noise field at the spawn position. */
	class FVelocityNoise : public FInitializerOp
	{
		int32 ControlPoint = 0;
		float TimeScale = 1.0f, SpatialScale = 0.01f, TimeOffset = 0.0f;
		FVector3f SpatialOffset = FVector3f::ZeroVector, AbsFlags = FVector3f::ZeroVector, InvertFlags = FVector3f::ZeroVector;
		FVector3f OutMin = FVector3f::ZeroVector, OutMax = FVector3f(1, 1, 1);
		bool bLocal = false;

		virtual void Configure() override
		{
			ControlPoint = I(TEXT("Control Point Number"), 0);
			TimeScale = F(TEXT("Time Noise Coordinate Scale"), 1.0f);
			SpatialScale = F(TEXT("Spatial Noise Coordinate Scale"), 0.01f);
			TimeOffset = F(TEXT("Time Coordinate Offset"), 0.0f);
			SpatialOffset = V(TEXT("Spatial Coordinate Offset"), FVector3f::ZeroVector);
			AbsFlags = V(TEXT("Absolute Value"), FVector3f::ZeroVector);
			InvertFlags = V(TEXT("Invert Abs Value"), FVector3f::ZeroVector);
			OutMin = V(TEXT("output minimum"), FVector3f::ZeroVector);
			OutMax = V(TEXT("output maximum"), FVector3f(1, 1, 1));
			bLocal = B(TEXT("Apply Velocity in Local Space (0/1)"), false);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FSourceParticleControlPoint& CP = Ctx.CP(ControlPoint);
			for (int32 i = Start; i < Start + Count; ++i)
			{
				const FVector3f Coordinate = P.GetVector(EAttr::Xyz, i) * SpatialScale + SpatialOffset
					+ FVector3f((Ctx.Time + TimeOffset) * TimeScale, 0.0f, 0.0f) + FVector3f((float)InstanceSeed, 0, 0);
				const FVector3f Noise = Noise3D(Coordinate);
				const FVector3f T(Fold(Noise.X, AbsFlags.X != 0.0f, InvertFlags.X != 0.0f), Fold(Noise.Y, AbsFlags.Y != 0.0f, InvertFlags.Y != 0.0f),
					Fold(Noise.Z, AbsFlags.Z != 0.0f, InvertFlags.Z != 0.0f));
				FVector3f Velocity(Lerp(OutMin.X, OutMax.X, T.X), Lerp(OutMin.Y, OutMax.Y, T.Y), Lerp(OutMin.Z, OutMax.Z, T.Z));
				if (bLocal)
				{
					Velocity = CP.TransformLocal(Velocity);
				}
				if (!Velocity.IsZero())
				{
					AddVelocity(P, Ctx, i, Velocity);
				}
			}
		}
	};

	/** Velocity Inherit from Control Point: the control point's own velocity, scaled. */
	class FVelocityInheritFromControlPoint : public FInitializerOp
	{
		int32 ControlPoint = 0;
		float Scale = 1.0f;

		virtual void Configure() override
		{
			ControlPoint = I(TEXT("control point number"), 0);
			Scale = F(TEXT("velocity scale"), 1.0f);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FVector3f Velocity = Ctx.CP(ControlPoint).Velocity(Ctx.DeltaTime) * Scale;
			if (Velocity.IsZero())
			{
				return;
			}
			for (int32 i = Start; i < Start + Count; ++i)
			{
				AddVelocity(P, Ctx, i, Velocity);
			}
		}
	};

	/** Velocity Set from Control Point: a control point's position (or its offset from another) as a velocity. */
	class FVelocitySetFromControlPoint : public FInitializerOp
	{
		int32 ControlPoint = 0, ComparisonCP = -1, LocalSpaceCP = -1;
		float Scale = 1.0f;
		bool bDirectionOnly = false;

		virtual void Configure() override
		{
			ControlPoint = I(TEXT("control point number"), 0);
			Scale = F(TEXT("velocity scale"), 1.0f);
			ComparisonCP = I(TEXT("comparison control point number"), -1);
			LocalSpaceCP = I(TEXT("local space control point number"), -1);
			bDirectionOnly = B(TEXT("direction only"), false);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			FVector3f Velocity = Ctx.CP(ControlPoint).Position;
			if (ComparisonCP >= 0 && ComparisonCP < FSourceParticleContext::MaxControlPoints)
			{
				Velocity -= Ctx.ControlPoints[ComparisonCP].Position;
			}
			if (bDirectionOnly)
			{
				Velocity = Velocity.GetSafeNormal();
			}
			Velocity *= Scale;
			if (LocalSpaceCP >= 0 && LocalSpaceCP < FSourceParticleContext::MaxControlPoints)
			{
				Velocity = Ctx.ControlPoints[LocalSpaceCP].TransformLocal(Velocity);
			}
			if (Velocity.IsZero())
			{
				return;
			}
			for (int32 i = Start; i < Start + Count; ++i)
			{
				AddVelocity(P, Ctx, i, Velocity);
			}
		}
	};

	/**
	 * Velocity Repulse from World (C_INIT_InitialRepulsionVelocity): probes the world along the six axes from
	 * the control point (or from each particle) and pushes away from whatever is close, so smoke does not sit in
	 * the floor. As in Source the push is written into the position: XYZ moves and PREV_XYZ does not, which
	 * the verlet step reads as a velocity; "Offset instead of accelerate" moves both.
	 */
	class FVelocityRepulseFromWorld : public FInitializerOp
	{
		FVector3f OutMin = FVector3f::ZeroVector, OutMax = FVector3f(1, 1, 1);
		int32 ControlPoint = 0;
		bool bPerParticle = false, bRadiusForTrace = false, bTranslate = false, bProportional = false;
		float TraceLength = 64.0f;

		virtual void Configure() override
		{
			OutMin = V(TEXT("minimum velocity"), FVector3f::ZeroVector);
			OutMax = V(TEXT("maximum velocity"), FVector3f(1, 1, 1));
			ControlPoint = I(TEXT("control_point_number"), 0);
			bPerParticle = B(TEXT("Per Particle World Collision Tests"), false);
			bRadiusForTrace = B(TEXT("Use radius for Per Particle Trace Length"), false);
			bTranslate = B(TEXT("Offset instead of accelerate"), false);
			bProportional = B(TEXT("Offset proportional to radius 0/1"), false);
			TraceLength = F(TEXT("Trace Length"), 64.0f);
		}

		FVector3f Probe(const FSourceParticleContext& Ctx, const FVector3f& From, float Length) const
		{
			static const FVector3f Axes[6] = { FVector3f(1, 0, 0), FVector3f(-1, 0, 0), FVector3f(0, 1, 0), FVector3f(0, -1, 0), FVector3f(0, 0, 1), FVector3f(0, 0, -1) };
			FVector3f ResultDirection = FVector3f::ZeroVector;
			float ResultForce = 0.0f;
			for (const FVector3f& Axis : Axes)
			{
				FSourceParticleTraceHit Hit;
				float Fraction = 1.0f;
				if (Ctx.TraceLine(From, From + Axis * Length, Hit) && Hit.bHit)
				{
					Fraction = Hit.Fraction;
				}
				// Push back in proportion to how close the probe came.
				ResultForce += 1.0f - Fraction;
				ResultDirection += -Axis * (1.0f - Fraction);
			}
			if (ResultDirection.IsZero())
			{
				return FVector3f::ZeroVector;	// nothing near: Source points up with zero force, which is no push
			}
			ResultDirection = ResultDirection.GetSafeNormal() * ResultForce;
			const FVector3f Amount(Lerp(OutMin.X, OutMax.X, ResultForce), Lerp(OutMin.Y, OutMax.Y, ResultForce), Lerp(OutMin.Z, OutMax.Z, ResultForce));
			return FVector3f(Amount.X * ResultDirection.X, Amount.Y * ResultDirection.Y, Amount.Z * ResultDirection.Z);
		}

		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			if (!Ctx.TraceLine)
			{
				return;
			}
			FVector3f Shared = FVector3f::ZeroVector;
			if (!bPerParticle)
			{
				Shared = Probe(Ctx, Ctx.CP(ControlPoint).Position, TraceLength);
			}
			for (int32 i = Start; i < Start + Count; ++i)
			{
				FVector3f Amount = Shared;
				if (bPerParticle)
				{
					const float Radius = P.GetFloat(EAttr::Radius, i);
					Amount = Probe(Ctx, P.GetVector(EAttr::Xyz, i), bRadiusForTrace ? Radius : TraceLength);
				}
				if (bProportional)
				{
					Amount *= P.GetFloat(EAttr::Radius, i);
				}
				if (Amount.IsZero())
				{
					continue;
				}
				P.SetVector(EAttr::Xyz, i, P.GetVector(EAttr::Xyz, i) + Amount);
				if (bTranslate)
				{
					P.SetVector(EAttr::PrevXyz, i, P.GetVector(EAttr::PrevXyz, i) + Amount);
				}
			}
		}
	};

	// ---- Scalars ------------------------------------------------------------------------------------------------

	/** Lifetime Random. */
	class FLifetimeRandom : public FInitializerOp
	{
		float Min = 0.0f, Max = 0.0f, Exp = 1.0f;
		virtual void Configure() override
		{
			Min = F(TEXT("lifetime_min"), 0.0f);
			Max = F(TEXT("lifetime_max"), 0.0f);
			Exp = F(TEXT("lifetime_random_exponent"), 1.0f);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			for (int32 i = Start; i < Start + Count; ++i)
			{
				P.SetFloat(EAttr::LifeDuration, i, RandRangeExp(P, Ctx, i, 0, Min, Max, Exp));
			}
		}
	};

	/** Radius Random. */
	class FRadiusRandom : public FInitializerOp
	{
		float Min = 1.0f, Max = 1.0f, Exp = 1.0f;
		virtual void Configure() override
		{
			Min = F(TEXT("radius_min"), 1.0f);
			Max = F(TEXT("radius_max"), 1.0f);
			Exp = F(TEXT("radius_random_exponent"), 1.0f);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			for (int32 i = Start; i < Start + Count; ++i)
			{
				P.SetFloat(EAttr::Radius, i, RandRangeExp(P, Ctx, i, 0, Min, Max, Exp));
			}
		}
	};

	/** Alpha Random: 0-255 in the file, 0-1 in the particle. */
	class FAlphaRandom : public FInitializerOp
	{
		float Min = 255.0f, Max = 255.0f, Exp = 1.0f;
		virtual void Configure() override
		{
			Min = (float)I(TEXT("alpha_min"), 255);
			Max = (float)I(TEXT("alpha_max"), 255);
			Exp = F(TEXT("alpha_random_exponent"), 1.0f);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			for (int32 i = Start; i < Start + Count; ++i)
			{
				P.SetFloat(EAttr::Alpha, i, RandRangeExp(P, Ctx, i, 0, Min / 255.0f, Max / 255.0f, Exp));
			}
		}
	};

	/** Color Random: one blend factor between the two colours. The lighting tint options are not simulated. */
	class FColorRandom : public FInitializerOp
	{
		FColor Color1 = FColor::White, Color2 = FColor::White;
		EAttr Output = EAttr::TintRgb;
		virtual void Configure() override
		{
			Color1 = C(TEXT("color1"), FColor::White);
			Color2 = C(TEXT("color2"), FColor::White);
			Output = I(TEXT("output field"), 6) == (int32)EAttr::GlowRgb ? EAttr::GlowRgb : EAttr::TintRgb;
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			for (int32 i = Start; i < Start + Count; ++i)
			{
				const float T = Rand(P, Ctx, i, 0);
				P.SetVector(Output, i, FVector3f(Lerp(Color1.R, Color2.R, T), Lerp(Color1.G, Color2.G, T), Lerp(Color1.B, Color2.B, T)) / 255.0f);
			}
		}
	};

	/** Color Lit Per Particle: the same blend; the world-lighting part is not simulated. */
	class FColorLitPerParticle : public FInitializerOp
	{
		FColor Color1 = FColor::White, Color2 = FColor::White;
		virtual void Configure() override
		{
			Color1 = C(TEXT("color1"), FColor::White);
			Color2 = C(TEXT("color2"), FColor::White);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			for (int32 i = Start; i < Start + Count; ++i)
			{
				const float T = Rand(P, Ctx, i, 0);
				P.SetVector(EAttr::TintRgb, i, FVector3f(Lerp(Color1.R, Color2.R, T), Lerp(Color1.G, Color2.G, T), Lerp(Color1.B, Color2.B, T)) / 255.0f);
			}
		}
	};

	/** Rotation Random: degrees in, radians out, with the optional coin-flip of direction. */
	class FRotationRandom : public FInitializerOp
	{
		float Initial = 0.0f, OffsetMin = 0.0f, OffsetMax = 360.0f, Exp = 1.0f;
		bool bFlip = true;
		virtual void Configure() override
		{
			Initial = F(TEXT("rotation_initial"), 0.0f);
			OffsetMin = F(TEXT("rotation_offset_min"), 0.0f);
			OffsetMax = F(TEXT("rotation_offset_max"), 360.0f);
			Exp = F(TEXT("rotation_random_exponent"), 1.0f);
			bFlip = B(TEXT("randomly_flip_direction"), true);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			for (int32 i = Start; i < Start + Count; ++i)
			{
				float Degrees = Initial + RandRangeExp(P, Ctx, i, 0, OffsetMin, OffsetMax, Exp);
				if (bFlip && Rand(P, Ctx, i, 3) < 0.5f)
				{
					Degrees = -Degrees;
				}
				P.SetFloat(EAttr::Rotation, i, Degrees * DegToRad);
			}
		}
	};

	/** Rotation Speed Random. */
	class FRotationSpeedRandom : public FInitializerOp
	{
		float Constant = 0.0f, Min = 0.0f, Max = 360.0f, Exp = 1.0f;
		bool bFlip = true;
		virtual void Configure() override
		{
			Constant = F(TEXT("rotation_speed_constant"), 0.0f);
			Min = F(TEXT("rotation_speed_random_min"), 0.0f);
			Max = F(TEXT("rotation_speed_random_max"), 360.0f);
			Exp = F(TEXT("rotation_speed_random_exponent"), 1.0f);
			bFlip = B(TEXT("randomly_flip_direction"), true);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			for (int32 i = Start; i < Start + Count; ++i)
			{
				float Degrees = Constant + RandRangeExp(P, Ctx, i, 0, Min, Max, Exp);
				if (bFlip && Rand(P, Ctx, i, 3) < 0.5f)
				{
					Degrees = -Degrees;
				}
				P.SetFloat(EAttr::RotationSpeed, i, Degrees * DegToRad);
			}
		}
	};

	/** Rotation Yaw Random. */
	class FRotationYawRandom : public FInitializerOp
	{
		float Initial = 0.0f, OffsetMin = 0.0f, OffsetMax = 360.0f, Exp = 1.0f;
		virtual void Configure() override
		{
			Initial = F(TEXT("yaw_initial"), 0.0f);
			OffsetMin = F(TEXT("yaw_offset_min"), 0.0f);
			OffsetMax = F(TEXT("yaw_offset_max"), 360.0f);
			Exp = F(TEXT("yaw_random_exponent"), 1.0f);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			for (int32 i = Start; i < Start + Count; ++i)
			{
				P.SetFloat(EAttr::Yaw, i, (Initial + RandRangeExp(P, Ctx, i, 0, OffsetMin, OffsetMax, Exp)) * DegToRad);
			}
		}
	};

	/** Rotation Yaw Flip Random: half a turn for a fraction of the particles. */
	class FRotationYawFlipRandom : public FInitializerOp
	{
		float Percentage = 0.5f;
		virtual void Configure() override { Percentage = F(TEXT("Flip Percentage"), 0.5f); }
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			for (int32 i = Start; i < Start + Count; ++i)
			{
				if (Rand(P, Ctx, i, 0) < Percentage)
				{
					P.SetFloat(EAttr::Yaw, i, P.GetFloat(EAttr::Yaw, i) + PI);
				}
			}
		}
	};

	/** Trail Length Random. */
	class FTrailLengthRandom : public FInitializerOp
	{
		float Min = 0.1f, Max = 0.1f, Exp = 1.0f;
		virtual void Configure() override
		{
			Min = F(TEXT("length_min"), 0.1f);
			Max = F(TEXT("length_max"), 0.1f);
			Exp = F(TEXT("length_random_exponent"), 1.0f);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			for (int32 i = Start; i < Start + Count; ++i)
			{
				P.SetFloat(EAttr::TrailLength, i, RandRangeExp(P, Ctx, i, 0, Min, Max, Exp));
			}
		}
	};

	/** Sequence Random: "linear" steps through the range; otherwise a random pick. */
	class FSequenceRandom : public FInitializerOp
	{
		int32 Min = 0, Max = 0;
		bool bLinear = false;
		virtual void Configure() override
		{
			Min = I(TEXT("sequence_min"), 0);
			Max = I(TEXT("sequence_max"), 0);
			bLinear = B(TEXT("linear"), false);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const int32 Span = FMath::Max(0, Max - Min);
			for (int32 i = Start; i < Start + Count; ++i)
			{
				const int32 Sequence = bLinear
					? Min + (Span > 0 ? P.IdOf(i) % (Span + 1) : 0)
					: (int32)RandRange(P, Ctx, i, 0, (float)Min, Max + 0.999f);
				P.SetFloat(EAttr::SequenceNumber, i, (float)Sequence);
			}
		}
	};

	/** Sequence Two Random. */
	class FSequenceTwoRandom : public FInitializerOp
	{
		int32 Min = 0, Max = 0;
		virtual void Configure() override
		{
			Min = I(TEXT("sequence_min"), 0);
			Max = I(TEXT("sequence_max"), 0);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			for (int32 i = Start; i < Start + Count; ++i)
			{
				P.SetFloat(EAttr::SequenceNumber1, i, (float)(int32)RandRange(P, Ctx, i, 0, (float)Min, Max + 0.999f));
			}
		}
	};

	/** Lifetime From Sequence: as long as the sheet sequence takes at the given frame rate. */
	class FLifetimeFromSequence : public FInitializerOp
	{
		float FPS = 30.0f;
		virtual void Configure() override { FPS = F(TEXT("Frames Per Second"), 30.0f); }
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			if (FPS <= 0.0f || Ctx.SheetSequenceFrameCounts.Num() == 0)
			{
				return;
			}
			for (int32 i = Start; i < Start + Count; ++i)
			{
				const int32 Sequence = FMath::Clamp((int32)P.GetFloat(EAttr::SequenceNumber, i), 0, Ctx.SheetSequenceFrameCounts.Num() - 1);
				P.SetFloat(EAttr::LifeDuration, i, Ctx.SheetSequenceFrameCounts[Sequence] / FPS);
			}
		}
	};

	/**
	 * Lifetime Pre-Age Noise (C_INIT_AgeNoise): starts each particle part-way through its life, by a noise
	 * value read at its creation time and position, so a burst is not all born at once.
	 */
	class FLifetimePreAgeNoise : public FInitializerOp
	{
		float TimeScale = 1.0f, SpatialScale = 1.0f, TimeOffset = 0.0f, AgeMin = 0.0f, AgeMax = 1.0f;
		FVector3f SpatialOffset = FVector3f::ZeroVector;
		bool bAbs = false, bInvert = false;

		virtual void Configure() override
		{
			TimeScale = F(TEXT("time noise coordinate scale"), 1.0f);
			SpatialScale = F(TEXT("spatial noise coordinate scale"), 1.0f);
			TimeOffset = F(TEXT("time coordinate offset"), 0.0f);
			SpatialOffset = V(TEXT("spatial coordinate offset"), FVector3f::ZeroVector);
			bAbs = B(TEXT("absolute value"), false);
			bInvert = B(TEXT("invert absolute value"), false);
			AgeMin = F(TEXT("start age minimum"), 0.0f);
			AgeMax = F(TEXT("start age maximum"), 1.0f);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const float AbsScale = bAbs ? 1.0f : 0.5f;
			const float ValueScale = AbsScale * (AgeMax - AgeMin);
			const float ValueBase = AgeMin + (1.0f - AbsScale) * (AgeMax - AgeMin);
			for (int32 i = Start; i < Start + Count; ++i)
			{
				const float CreationTime = P.GetFloat(EAttr::CreationTime, i);
				const FVector3f Loc = (P.GetVector(EAttr::Xyz, i) + SpatialOffset) * SpatialScale;
				const FVector3f Coord = FVector3f((CreationTime + TimeOffset) * TimeScale) + Loc + FVector3f((float)InstanceSeed, 0, 0);
				float Noise = Noise3D(Coord).X;
				if (bAbs)
				{
					Noise = FMath::Abs(Noise);
				}
				if (bInvert)
				{
					Noise = 1.0f - Noise;
				}
				const float InitialAge = Clamp01(ValueBase + ValueScale * Noise) * P.GetFloat(EAttr::LifeDuration, i);
				P.SetFloat(EAttr::CreationTime, i, CreationTime - InitialAge);
			}
		}
	};

	/** Scalar Random: a random into any scalar field. */
	class FScalarRandom : public FInitializerOp
	{
		float Min = 0.0f, Max = 0.0f, Exp = 1.0f;
		EAttr Field = EAttr::Radius;
		virtual void Configure() override
		{
			Min = F(TEXT("min"), 0.0f);
			Max = F(TEXT("max"), 0.0f);
			Exp = F(TEXT("exponent"), 1.0f);
			Field = SourceParticleAttr::FromField(I(TEXT("output field"), 3), EAttr::Radius);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			for (int32 i = Start; i < Start + Count; ++i)
			{
				P.SetFloat(Field, i, RandRangeExp(P, Ctx, i, 0, Min, Max, Exp));
			}
		}
	};

	/** Vector Random. */
	class FVectorRandom : public FInitializerOp
	{
		FVector3f Min = FVector3f::ZeroVector, Max = FVector3f::ZeroVector;
		EAttr Field = EAttr::Xyz;
		virtual void Configure() override
		{
			Min = V(TEXT("min"), FVector3f::ZeroVector);
			Max = V(TEXT("max"), FVector3f::ZeroVector);
			Field = SourceParticleAttr::FromField(I(TEXT("output field"), 0), EAttr::Xyz);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			for (int32 i = Start; i < Start + Count; ++i)
			{
				P.SetVector(Field, i, RandVector(P, Ctx, i, 0, Min, Max));
			}
		}
	};

	/** Vector Component Random. */
	class FVectorComponentRandom : public FInitializerOp
	{
		float Min = 0.0f, Max = 0.0f;
		int32 Comp = 0;
		EAttr Field = EAttr::Xyz;
		virtual void Configure() override
		{
			Min = F(TEXT("min"), 0.0f);
			Max = F(TEXT("max"), 0.0f);
			Comp = FMath::Clamp(I(TEXT("component 0/1/2 X/Y/Z"), 0), 0, 2);
			Field = SourceParticleAttr::FromField(I(TEXT("output field"), 0), EAttr::Xyz);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			for (int32 i = Start; i < Start + Count; ++i)
			{
				P.SetFloat(Field, i, RandRange(P, Ctx, i, 0, Min, Max), Comp);
			}
		}
	};

	// ---- Remaps -------------------------------------------------------------------------------------------------

	/** Remap Initial Scalar: one scalar field into another at spawn; or the system age over the emitter window. */
	class FRemapInitialScalar : public FInitializerOp
	{
		float LifeStart = -1.0f, LifeEnd = -1.0f, InMin = 0.0f, InMax = 1.0f, OutMin = 0.0f, OutMax = 1.0f;
		EAttr Input = EAttr::CreationTime, Output = EAttr::Radius;
		bool bScaleInitial = false, bOnlyInRange = false;
		virtual void Configure() override
		{
			LifeStart = F(TEXT("emitter lifetime start time (seconds)"), -1.0f);
			LifeEnd = F(TEXT("emitter lifetime end time (seconds)"), -1.0f);
			Input = SourceParticleAttr::FromField(I(TEXT("input field"), 8), EAttr::CreationTime);
			InMin = F(TEXT("input minimum"), 0.0f);
			InMax = F(TEXT("input maximum"), 1.0f);
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 3), EAttr::Radius);
			OutMin = F(TEXT("output minimum"), 0.0f);
			OutMax = F(TEXT("output maximum"), 1.0f);
			bScaleInitial = B(TEXT("output is scalar of initial random range"), false);
			bOnlyInRange = B(TEXT("only active within specified input range"), false);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const bool bWindow = LifeStart >= 0.0f && LifeEnd >= 0.0f;
			for (int32 i = Start; i < Start + Count; ++i)
			{
				float InputValue;
				if (bWindow)
				{
					InputValue = Lerp(InMin, InMax, RemapClamped(Ctx.Time, LifeStart, LifeEnd));
				}
				else
				{
					InputValue = P.GetFloat(Input, i);
				}
				if (bOnlyInRange && (InputValue < InMin || InputValue > InMax))
				{
					continue;
				}
				const float Result = Lerp(OutMin, OutMax, RemapClamped(InputValue, InMin, InMax));
				P.SetFloat(Output, i, bScaleInitial ? P.GetFloat(Output, i) * Result : Result);
			}
		}
	};

	/** Remap Control Point to Scalar: one component of a control point into a scalar field at spawn. */
	class FRemapControlPointToScalar : public FInitializerOp
	{
		int32 InputCP = 0, InputField = 0;
		float InMin = 0.0f, InMax = 1.0f, OutMin = 0.0f, OutMax = 1.0f;
		EAttr Output = EAttr::Radius;
		bool bScaleInitial = false;
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
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const float Result = Lerp(OutMin, OutMax, RemapClamped(Ctx.CPField(InputCP, InputField), InMin, InMax));
			for (int32 i = Start; i < Start + Count; ++i)
			{
				P.SetFloat(Output, i, bScaleInitial ? P.GetFloat(Output, i) * Result : Result);
			}
		}
	};

	/** Remap Control Point to Vector: a control point's position, per axis, into a vector field or a position offset. */
	class FRemapControlPointToVector : public FInitializerOp
	{
		int32 InputCP = 0;
		FVector3f InMin = FVector3f::ZeroVector, InMax = FVector3f::ZeroVector, OutMin = FVector3f::ZeroVector, OutMax = FVector3f::ZeroVector;
		EAttr Output = EAttr::Xyz;
		bool bOffsetPosition = false;
		virtual void Configure() override
		{
			InputCP = I(TEXT("input control point number"), 0);
			InMin = V(TEXT("input minimum"), FVector3f::ZeroVector);
			InMax = V(TEXT("input maximum"), FVector3f::ZeroVector);
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 0), EAttr::Xyz);
			OutMin = V(TEXT("output minimum"), FVector3f::ZeroVector);
			OutMax = V(TEXT("output maximum"), FVector3f::ZeroVector);
			bOffsetPosition = B(TEXT("offset position"), false);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FVector3f Pos = Ctx.CP(InputCP).Position;
			const FVector3f Result(Lerp(OutMin.X, OutMax.X, RemapClamped(Pos.X, InMin.X, InMax.X)),
				Lerp(OutMin.Y, OutMax.Y, RemapClamped(Pos.Y, InMin.Y, InMax.Y)),
				Lerp(OutMin.Z, OutMax.Z, RemapClamped(Pos.Z, InMin.Z, InMax.Z)));
			for (int32 i = Start; i < Start + Count; ++i)
			{
				if (bOffsetPosition)
				{
					OffsetPosition(P, i, Result);
				}
				else
				{
					P.SetVector(Output, i, Result);
				}
			}
		}
	};

	/** Remap Scalar to Vector: one scalar field into a vector field, per axis, optionally in a control point's frame. */
	class FRemapScalarToVector : public FInitializerOp
	{
		float LifeStart = -1.0f, LifeEnd = -1.0f, InMin = 0.0f, InMax = 1.0f;
		FVector3f OutMin = FVector3f::ZeroVector, OutMax = FVector3f(1, 1, 1);
		EAttr Input = EAttr::CreationTime, Output = EAttr::Xyz;
		bool bScaleInitial = false, bLocal = true;
		int32 ControlPoint = 0;
		virtual void Configure() override
		{
			LifeStart = F(TEXT("emitter lifetime start time (seconds)"), -1.0f);
			LifeEnd = F(TEXT("emitter lifetime end time (seconds)"), -1.0f);
			Input = SourceParticleAttr::FromField(I(TEXT("input field"), 8), EAttr::CreationTime);
			InMin = F(TEXT("input minimum"), 0.0f);
			InMax = F(TEXT("input maximum"), 1.0f);
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 0), EAttr::Xyz);
			OutMin = V(TEXT("output minimum"), FVector3f::ZeroVector);
			OutMax = V(TEXT("output maximum"), FVector3f(1, 1, 1));
			bScaleInitial = B(TEXT("output is scalar of initial random range"), false);
			bLocal = B(TEXT("use local system"), true);
			ControlPoint = I(TEXT("control_point_number"), 0);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FSourceParticleControlPoint& CP = Ctx.CP(ControlPoint);
			const bool bWindow = LifeStart >= 0.0f && LifeEnd >= 0.0f;
			for (int32 i = Start; i < Start + Count; ++i)
			{
				const float InputValue = bWindow ? Lerp(InMin, InMax, RemapClamped(Ctx.Time, LifeStart, LifeEnd)) : P.GetFloat(Input, i);
				const float T = RemapClamped(InputValue, InMin, InMax);
				FVector3f Result = FMath::Lerp(OutMin, OutMax, T);
				if (bLocal)
				{
					Result = CP.TransformLocal(Result);
				}
				if (bScaleInitial)
				{
					Result *= P.GetInitialVector(Output, i);
				}
				P.SetVector(Output, i, Result);
			}
		}
	};

	/** Offset Vector to Vector: an input vector field plus a random offset, into an output vector field. */
	class FOffsetVectorToVector : public FInitializerOp
	{
		EAttr Input = EAttr::Xyz, Output = EAttr::Xyz;
		FVector3f OffsetMin = FVector3f::ZeroVector, OffsetMax = FVector3f(1, 1, 1);
		virtual void Configure() override
		{
			Input = SourceParticleAttr::FromField(I(TEXT("input field"), 0), EAttr::Xyz);
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 0), EAttr::Xyz);
			OffsetMin = V(TEXT("output offset minimum"), FVector3f::ZeroVector);
			OffsetMax = V(TEXT("output offset maximum"), FVector3f(1, 1, 1));
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			for (int32 i = Start; i < Start + Count; ++i)
			{
				P.SetVector(Output, i, P.GetVector(Input, i) + RandVector(P, Ctx, i, 0, OffsetMin, OffsetMax));
			}
		}
	};

	/** Normal Align to CP: the normal points away from the control point. */
	class FNormalAlignToCP : public FInitializerOp
	{
		int32 ControlPoint = 0;
		virtual void Configure() override { ControlPoint = I(TEXT("control_point_number"), 0); }
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FSourceParticleControlPoint& CP = Ctx.CP(ControlPoint);
			for (int32 i = Start; i < Start + Count; ++i)
			{
				const FVector3f Dir = P.GetVector(EAttr::Xyz, i) - CP.Position;
				P.SetVector(EAttr::Normal, i, Dir.SizeSquared() > 1e-12f ? Dir.GetSafeNormal() : CP.Up);
			}
		}
	};

	/** Normal Modify Offset Random. */
	class FNormalModifyOffsetRandom : public FInitializerOp
	{
		int32 ControlPoint = 0;
		FVector3f OffsetMin = FVector3f::ZeroVector, OffsetMax = FVector3f::ZeroVector;
		bool bLocal = false, bNormalize = false;
		virtual void Configure() override
		{
			ControlPoint = I(TEXT("control_point_number"), 0);
			OffsetMin = V(TEXT("offset min"), FVector3f::ZeroVector);
			OffsetMax = V(TEXT("offset max"), FVector3f::ZeroVector);
			bLocal = B(TEXT("offset in local space 0/1"), false);
			bNormalize = B(TEXT("normalize output 0/1"), false);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FSourceParticleControlPoint& CP = Ctx.CP(ControlPoint);
			for (int32 i = Start; i < Start + Count; ++i)
			{
				FVector3f Offset = RandVector(P, Ctx, i, 0, OffsetMin, OffsetMax);
				if (bLocal)
				{
					Offset = CP.TransformLocal(Offset);
				}
				FVector3f Normal = P.GetVector(EAttr::Normal, i) + Offset;
				if (bNormalize && Normal.SizeSquared() > 1e-8f)
				{
					Normal.Normalize();
				}
				P.SetVector(EAttr::Normal, i, Normal);
			}
		}
	};

	/** Remap Noise to Scalar: a noise value at the spawn position and time into a scalar field. */
	class FRemapNoiseToScalar : public FInitializerOp
	{
		float TimeScale = 0.1f, SpatialScale = 0.001f, TimeOffset = 0.0f, OutMin = 0.0f, OutMax = 1.0f;
		FVector3f SpatialOffset = FVector3f::ZeroVector;
		EAttr Output = EAttr::Radius;
		bool bAbs = false, bInvert = false;
		virtual void Configure() override
		{
			TimeScale = F(TEXT("time noise coordinate scale"), 0.1f);
			SpatialScale = F(TEXT("spatial noise coordinate scale"), 0.001f);
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 3), EAttr::Radius);
			TimeOffset = F(TEXT("time coordinate offset"), 0.0f);
			SpatialOffset = V(TEXT("spatial coordinate offset"), FVector3f::ZeroVector);
			bAbs = B(TEXT("absolute value"), false);
			bInvert = B(TEXT("invert absolute value"), false);
			OutMin = F(TEXT("output minimum"), 0.0f);
			OutMax = F(TEXT("output maximum"), 1.0f);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			for (int32 i = Start; i < Start + Count; ++i)
			{
				const FVector3f Sp = P.GetVector(EAttr::Xyz, i) * SpatialScale + SpatialOffset;
				const float Coordinate = Sp.X + Sp.Y * 57.0f + Sp.Z * 131.0f + (Ctx.Time + TimeOffset) * TimeScale + InstanceSeed;
				const float T = Fold(Noise1D(Coordinate), bAbs, bInvert);
				P.SetFloat(Output, i, Lerp(OutMin, OutMax, T));
			}
		}
	};

	/** Remap Particle Count to Scalar: the particle's ordinal into a scalar field. */
	class FRemapParticleCountToScalar : public FInitializerOp
	{
		int32 InMin = 0, InMax = 10;
		float OutMin = 0.0f, OutMax = 1.0f;
		EAttr Output = EAttr::Radius;
		bool bScaleInitial = false, bOnlyInRange = false;
		virtual void Configure() override
		{
			InMin = I(TEXT("input minimum"), 0);
			InMax = I(TEXT("input maximum"), 10);
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 3), EAttr::Radius);
			OutMin = F(TEXT("output minimum"), 0.0f);
			OutMax = F(TEXT("output maximum"), 1.0f);
			bScaleInitial = B(TEXT("output is scalar of initial random range"), false);
			bOnlyInRange = B(TEXT("only active within specified input range"), false);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			for (int32 i = Start; i < Start + Count; ++i)
			{
				const int32 Id = P.IdOf(i);
				if (bOnlyInRange && (Id < InMin || Id > InMax))
				{
					continue;
				}
				float Value = Lerp(OutMin, OutMax, RemapClamped((float)Id, (float)InMin, (float)InMax));
				if (bScaleInitial)
				{
					Value *= P.GetFloat(Output, i);
				}
				P.SetFloat(Output, i, Value);
			}
		}
	};

	/** Remap Speed to Scalar (initializer): the particle's (or a control point's) speed into a scalar field. */
	class FRemapSpeedToScalarInit : public FInitializerOp
	{
		int32 ControlPoint = 0;
		bool bPerParticle = false, bScaleInitial = false;
		float InMin = 0.0f, InMax = 1.0f, OutMin = 0.0f, OutMax = 1.0f;
		EAttr Output = EAttr::Radius;
		virtual void Configure() override
		{
			ControlPoint = I(TEXT("control point number (ignored if per particle)"), 0);
			bPerParticle = B(TEXT("per particle"), false);
			InMin = F(TEXT("input minimum"), 0.0f);
			InMax = F(TEXT("input maximum"), 1.0f);
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 3), EAttr::Radius);
			OutMin = F(TEXT("output minimum"), 0.0f);
			OutMax = F(TEXT("output maximum"), 1.0f);
			bScaleInitial = B(TEXT("output is scalar of initial random range"), false);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const float Dt = Ctx.DeltaTime > 0.0f ? Ctx.DeltaTime : (1.0f / 60.0f);
			const float CPSpeed = Ctx.CP(ControlPoint).Velocity(Ctx.DeltaTime).Size();
			for (int32 i = Start; i < Start + Count; ++i)
			{
				const float Speed = bPerParticle ? ((P.GetVector(EAttr::Xyz, i) - P.GetVector(EAttr::PrevXyz, i)) / Dt).Size() : CPSpeed;
				const float Result = Lerp(OutMin, OutMax, RemapClamped(Speed, InMin, InMax));
				P.SetFloat(Output, i, bScaleInitial ? P.GetFloat(Output, i) * Result : Result);
			}
		}
	};

	/** Remap Initial Direction to CP to Vector. */
	class FRemapInitialDirectionToCPToVector : public FInitializerOp
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
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FVector3f CPPos = Ctx.CP(ControlPoint).Position;
			for (int32 i = Start; i < Start + Count; ++i)
			{
				P.SetVector(Output, i, DirectionToCP(CPPos, P.GetVector(EAttr::Xyz, i), bNormalize, OffsetAxis, OffsetRotation, Scale));
			}
		}
	};

	/** Remap CP Orientation to Rotation: the control point's heading, in a chosen plane, as a rotation. */
	class FRemapCPOrientationToRotation : public FInitializerOp
	{
		int32 ControlPoint = 0, Axis = 0;
		EAttr Field = EAttr::Rotation;
		float OffsetRotation = 0.0f;
		virtual void Configure() override
		{
			ControlPoint = I(TEXT("control point"), 0);
			Field = I(TEXT("rotation field"), 0) == 12 ? EAttr::Yaw : EAttr::Rotation;
			Axis = I(TEXT("axis"), 0);
			OffsetRotation = F(TEXT("offset rotation"), 0.0f);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FVector3f Fwd = Ctx.CP(ControlPoint).Forward;
			float Angle = Axis == 1 ? FMath::Atan2(Fwd.Z, Fwd.Y) : (Axis == 2 ? FMath::Atan2(Fwd.X, Fwd.Z) : FMath::Atan2(Fwd.Y, Fwd.X));
			Angle += OffsetRotation * DegToRad;
			for (int32 i = Start; i < Start + Count; ++i)
			{
				P.SetFloat(Field, i, Angle);
			}
		}
	};

	/** Remap Initial Distance to Control Point to Scalar. The line-of-sight options are not simulated. */
	class FRemapInitialDistanceToControlPointToScalar : public FInitializerOp
	{
		float DistMin = 0.0f, DistMax = 128.0f, OutMin = 0.0f, OutMax = 1.0f;
		EAttr Output = EAttr::Radius;
		int32 ControlPoint = 0;
		bool bScaleInitial = false, bOnlyInRange = false;
		virtual void Configure() override
		{
			DistMin = F(TEXT("distance minimum"), 0.0f);
			DistMax = F(TEXT("distance maximum"), 128.0f);
			Output = SourceParticleAttr::FromField(I(TEXT("output field"), 3), EAttr::Radius);
			OutMin = F(TEXT("output minimum"), 0.0f);
			OutMax = F(TEXT("output maximum"), 1.0f);
			ControlPoint = I(TEXT("control point"), 0);
			bScaleInitial = B(TEXT("output is scalar of initial random range"), false);
			bOnlyInRange = B(TEXT("only active within specified distance"), false);
		}
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FVector3f CPPos = Ctx.CP(ControlPoint).Position;
			for (int32 i = Start; i < Start + Count; ++i)
			{
				const float Distance = (P.GetVector(EAttr::Xyz, i) - CPPos).Size();
				if (bOnlyInRange && (Distance < DistMin || Distance > DistMax))
				{
					continue;
				}
				const float Result = Lerp(OutMin, OutMax, RemapClamped(Distance, DistMin, DistMax));
				P.SetFloat(Output, i, bScaleInitial ? P.GetFloat(Output, i) * Result : Result);
			}
		}
	};

	/** Inherit Initial Value From Parent Particle: a field copied from one of the parent's particles. */
	class FInheritInitialValueFromParentParticle : public FInitializerOp
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
		virtual void InitNewParticles(FSourceParticleCollection& P, FSourceParticleContext& Ctx, int32 Start, int32 Count, float Strength) override
		{
			const FSourceParticleCollection* Parent = Ctx.ParentParticles;
			if (!Parent || Parent->Num() == 0)
			{
				return;
			}
			const int32 Comps = SourceParticleAttr::Components(Field);
			for (int32 i = Start; i < Start + Count; ++i)
			{
				const int32 Source = bRandom
					? FMath::Min((int32)(Rand(P, Ctx, i, 0) * Parent->Num()), Parent->Num() - 1)
					: (P.IdOf(i) * Increment) % Parent->Num();
				for (int32 c = 0; c < Comps; ++c)
				{
					P.SetFloat(Field, i, Parent->GetFloat(Field, Source, c) * Scale, c);
				}
			}
		}
	};
}

void RegisterSourceParticleInitializerOps(FSourceParticleOpRegistry& R)
{
	LAMBDA_PARTICLE_OP(R, "initializers", "Position Within Sphere Random", FPositionWithinSphereRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Position Within Box Random", FPositionWithinBoxRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Position Modify Offset Random", FPositionModifyOffsetRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Position Modify Warp Random", FPositionModifyWarpRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Position Modify Place On Ground", FPositionModifyPlaceOnGround);
	LAMBDA_PARTICLE_OP(R, "initializers", "Position Along Ring", FPositionAlongRing);
	LAMBDA_PARTICLE_OP(R, "initializers", "Position From Parent Particles", FPositionFromParentParticles);
	LAMBDA_PARTICLE_OP(R, "initializers", "Position from Parent Cache", FPositionFromParentCache);
	LAMBDA_PARTICLE_OP(R, "initializers", "Position Along Path Random", FPositionAlongPathRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Position Along Path Sequential", FPositionAlongPathSequential);
	LAMBDA_PARTICLE_OP(R, "initializers", "Position In CP Hierarchy", FPositionInCPHierarchy);
	LAMBDA_PARTICLE_OP(R, "initializers", "Move Particles Between 2 Control Points", FMoveParticlesBetween2ControlPoints);
	LAMBDA_PARTICLE_OP(R, "initializers", "Position Along Epitrochoid", FPositionAlongEpitrochoid);
	LAMBDA_PARTICLE_OP(R, "initializers", "Position From Chaotic Attractor", FPositionFromChaoticAttractor);
	LAMBDA_PARTICLE_OP(R, "initializers", "Velocity Random", FVelocityRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Velocity Noise", FVelocityNoise);
	LAMBDA_PARTICLE_OP(R, "initializers", "Velocity Inherit from Control Point", FVelocityInheritFromControlPoint);
	LAMBDA_PARTICLE_OP(R, "initializers", "Velocity Set from Control Point", FVelocitySetFromControlPoint);
	LAMBDA_PARTICLE_OP(R, "initializers", "Velocity Repulse from World", FVelocityRepulseFromWorld);
	LAMBDA_PARTICLE_OP(R, "initializers", "Lifetime Random", FLifetimeRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Radius Random", FRadiusRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Alpha Random", FAlphaRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Color Random", FColorRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Color Lit Per Particle", FColorLitPerParticle);
	LAMBDA_PARTICLE_OP(R, "initializers", "Rotation Random", FRotationRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Rotation Speed Random", FRotationSpeedRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Rotation Yaw Random", FRotationYawRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Rotation Yaw Flip Random", FRotationYawFlipRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Trail Length Random", FTrailLengthRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Sequence Random", FSequenceRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Sequence Two Random", FSequenceTwoRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Lifetime From Sequence", FLifetimeFromSequence);
	LAMBDA_PARTICLE_OP(R, "initializers", "Lifetime Pre-Age Noise", FLifetimePreAgeNoise);
	LAMBDA_PARTICLE_OP(R, "initializers", "Scalar Random", FScalarRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Vector Random", FVectorRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Vector Component Random", FVectorComponentRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Remap Initial Scalar", FRemapInitialScalar);
	LAMBDA_PARTICLE_OP(R, "initializers", "Remap Control Point to Scalar", FRemapControlPointToScalar);
	LAMBDA_PARTICLE_OP(R, "initializers", "Remap Control Point to Vector", FRemapControlPointToVector);
	LAMBDA_PARTICLE_OP(R, "initializers", "Remap Scalar to Vector", FRemapScalarToVector);
	LAMBDA_PARTICLE_OP(R, "initializers", "Offset Vector to Vector", FOffsetVectorToVector);
	LAMBDA_PARTICLE_OP(R, "initializers", "Normal Align to CP", FNormalAlignToCP);
	LAMBDA_PARTICLE_OP(R, "initializers", "Normal Modify Offset Random", FNormalModifyOffsetRandom);
	LAMBDA_PARTICLE_OP(R, "initializers", "Remap Noise to Scalar", FRemapNoiseToScalar);
	LAMBDA_PARTICLE_OP(R, "initializers", "Remap Particle Count to Scalar", FRemapParticleCountToScalar);
	LAMBDA_PARTICLE_OP(R, "initializers", "Remap Speed to Scalar", FRemapSpeedToScalarInit);
	LAMBDA_PARTICLE_OP(R, "initializers", "Remap Initial Direction to CP to Vector", FRemapInitialDirectionToCPToVector);
	LAMBDA_PARTICLE_OP(R, "initializers", "Remap CP Orientation to Rotation", FRemapCPOrientationToRotation);
	LAMBDA_PARTICLE_OP(R, "initializers", "Remap Initial Distance to Control Point to Scalar", FRemapInitialDistanceToControlPointToScalar);
	LAMBDA_PARTICLE_OP(R, "initializers", "Inherit Initial Value From Parent Particle", FInheritInitialValueFromParentParticle);
}
