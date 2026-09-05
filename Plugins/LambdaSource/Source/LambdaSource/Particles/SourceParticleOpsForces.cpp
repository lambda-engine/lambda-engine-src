#include "Particles/SourceParticleOpsInternal.h"
#include "Core/LambdaSourceModule.h"

// The forces (particles/builtin_particle_forces.cpp) and constraints (builtin_constraints.cpp), which only run
// from inside Movement Basic; the renderers, which simulate nothing; and the registry that hands them all out.

using namespace SourceParticleOpHelpers;
using EAttr = ESourceParticleAttr;

namespace
{
	// ---- Forces -------------------------------------------------------------------------------------------------

	/** random force: a fresh random acceleration for every particle every step. */
	class FRandomForce : public FSourceParticleOp
	{
		FVector3f MinForce = FVector3f::ZeroVector, MaxForce = FVector3f::ZeroVector;
		virtual void Configure() override
		{
			MinForce = V(TEXT("min force"), FVector3f::ZeroVector);
			MaxForce = V(TEXT("max force"), FVector3f::ZeroVector);
		}
		virtual void AddForces(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength, TArray<FVector3f>& Accel) override
		{
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const int32 Sample = P.IdOf(i) + Ctx.OperatorRandomOffset;
				Accel[i] += FVector3f(FSourceParticleRandom::Range(InstanceSeed * 31, Sample, MinForce.X, MaxForce.X),
					FSourceParticleRandom::Range(InstanceSeed * 31 + 1, Sample, MinForce.Y, MaxForce.Y),
					FSourceParticleRandom::Range(InstanceSeed * 31 + 2, Sample, MinForce.Z, MaxForce.Z)) * Strength;
			}
		}
	};

	/** twist around axis: a vortex around an axis through control point 0. */
	class FTwistAroundAxis : public FSourceParticleOp
	{
		float Amount = 0.0f;
		FVector3f Axis = FVector3f(0, 0, 1);
		bool bLocal = false;
		virtual void Configure() override
		{
			Amount = F(TEXT("amount of force"), 0.0f);
			Axis = V(TEXT("twist axis"), FVector3f(0, 0, 1));
			bLocal = B(TEXT("object local space axis 0/1"), false);
		}
		virtual void AddForces(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength, TArray<FVector3f>& Accel) override
		{
			if (Amount == 0.0f)
			{
				return;
			}
			const FSourceParticleControlPoint& CP0 = Ctx.CP(0);
			FVector3f A = bLocal ? CP0.TransformLocal(Axis) : Axis;
			if (A.SizeSquared() < 1e-8f)
			{
				return;
			}
			A.Normalize();
			const float Force = Amount * Strength;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				FVector3f Radial = P.GetVector(EAttr::Xyz, i) - CP0.Position;
				Radial -= A * FVector3f::DotProduct(Radial, A);
				const float Length = Radial.Size();
				if (Length < 1e-6f)
				{
					continue;
				}
				Accel[i] += FVector3f::CrossProduct(Radial / Length, A) * Force;
			}
		}
	};

	/** Pull towards control point: an inverse-power attraction (negative repels). */
	class FPullTowardsControlPoint : public FSourceParticleOp
	{
		float Amount = 0.0f, FalloffPower = 2.0f;
		int32 ControlPoint = 0;
		virtual void Configure() override
		{
			Amount = F(TEXT("amount of force"), 0.0f);
			FalloffPower = F(TEXT("falloff power"), 2.0f);
			ControlPoint = I(TEXT("control point number"), 0);
		}
		virtual void AddForces(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength, TArray<FVector3f>& Accel) override
		{
			if (Amount == 0.0f)
			{
				return;
			}
			const FVector3f CPPos = Ctx.CP(ControlPoint).Position;
			const float Force = Amount * Strength;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const FVector3f ToCP = CPPos - P.GetVector(EAttr::Xyz, i);
				const float Distance = ToCP.Size();
				if (Distance < 1e-4f)
				{
					continue;
				}
				const float Falloff = FMath::Pow(Distance, FalloffPower);
				if (Falloff < 1e-8f)
				{
					continue;
				}
				Accel[i] += ToCP / Distance * (Force / Falloff);
			}
		}
	};

	/** time varying force: one acceleration blending into another over a particle-age window. */
	class FTimeVaryingForce : public FSourceParticleOp
	{
		float StartTime = 0.0f, EndTime = 10.0f;
		FVector3f StartForce = FVector3f::ZeroVector, EndForce = FVector3f::ZeroVector;
		virtual void Configure() override
		{
			StartTime = F(TEXT("time to start transition"), 0.0f);
			StartForce = V(TEXT("starting force"), FVector3f::ZeroVector);
			EndTime = F(TEXT("time to end transition"), 10.0f);
			EndForce = V(TEXT("ending force"), FVector3f::ZeroVector);
		}
		virtual void AddForces(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength, TArray<FVector3f>& Accel) override
		{
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float T = RemapClamped(Age(P, Ctx, i), StartTime, EndTime);
				Accel[i] += FMath::Lerp(StartForce, EndForce, T) * Strength;
			}
		}
	};

	/** turbulent force: a four-octave noise field; the strength is ignored, as in Source. */
	class FTurbulentForce : public FSourceParticleOp
	{
		float Scales[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
		FVector3f Amounts[4] = { FVector3f(1, 1, 1), FVector3f(0.5f, 0.5f, 0.5f), FVector3f(0.25f, 0.25f, 0.25f), FVector3f(0.125f, 0.125f, 0.125f) };
		virtual void Configure() override
		{
			Scales[0] = F(TEXT("Noise scale 0"), 1.0f);
			Amounts[0] = V(TEXT("Noise amount 0"), FVector3f(1, 1, 1));
			Scales[1] = F(TEXT("Noise scale 1"), 0.0f);
			Amounts[1] = V(TEXT("Noise amount 1"), FVector3f(0.5f, 0.5f, 0.5f));
			Scales[2] = F(TEXT("Noise scale 2"), 0.0f);
			Amounts[2] = V(TEXT("Noise amount 2"), FVector3f(0.25f, 0.25f, 0.25f));
			Scales[3] = F(TEXT("Noise scale 3"), 0.0f);
			Amounts[3] = V(TEXT("Noise amount 3"), FVector3f(0.125f, 0.125f, 0.125f));
		}
		virtual void AddForces(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength, TArray<FVector3f>& Accel) override
		{
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const FVector3f Position = P.GetVector(EAttr::Xyz, i);
				FVector3f Total = FVector3f::ZeroVector;
				for (int32 Octave = 0; Octave < 4; ++Octave)
				{
					if (Amounts[Octave].IsZero())
					{
						continue;
					}
					const FVector3f Noise = Noise3D(Position * Scales[Octave] + FVector3f(Octave * 37.7f, 0, 0));
					Total += FVector3f(Noise.X * Amounts[Octave].X, Noise.Y * Amounts[Octave].Y, Noise.Z * Amounts[Octave].Z);
				}
				Accel[i] += Total;
			}
		}
	};

	/** Force based on distance from plane. */
	class FForceBasedOnDistanceFromPlane : public FSourceParticleOp
	{
		float MinDist = 0.0f, MaxDist = 1.0f, Exponent = 1.0f;
		FVector3f ForceAtMin = FVector3f::ZeroVector, ForceAtMax = FVector3f::ZeroVector, Normal = FVector3f(0, 0, 1);
		int32 ControlPoint = 0;
		virtual void Configure() override
		{
			MinDist = F(TEXT("Min distance from plane"), 0.0f);
			ForceAtMin = V(TEXT("Force at Min distance"), FVector3f::ZeroVector);
			MaxDist = F(TEXT("Max Distance from plane"), 1.0f);
			ForceAtMax = V(TEXT("Force at Max distance"), FVector3f::ZeroVector);
			Normal = V(TEXT("Plane Normal"), FVector3f(0, 0, 1));
			ControlPoint = I(TEXT("Control point number"), 0);
			Exponent = F(TEXT("Exponent"), 1.0f);
		}
		virtual void AddForces(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength, TArray<FVector3f>& Accel) override
		{
			if (Normal.SizeSquared() < 1e-8f)
			{
				return;
			}
			const FVector3f N = Normal.GetSafeNormal();
			const FVector3f CPPos = Ctx.CP(ControlPoint).Position;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const float Distance = FVector3f::DotProduct(P.GetVector(EAttr::Xyz, i) - CPPos, N);
				float T = RemapClamped(Distance, MinDist, MaxDist);
				if (Exponent != 1.0f)
				{
					T = FMath::Pow(T, Exponent);
				}
				Accel[i] += FMath::Lerp(ForceAtMin, ForceAtMax, T) * Strength;
			}
		}
	};

	/** Create vortices from parent particles: every parent particle is a vortex centre. */
	class FCreateVorticesFromParentParticles : public FSourceParticleOp
	{
		float Amount = 0.0f;
		FVector3f Axis = FVector3f(0, 0, 1);
		virtual void Configure() override
		{
			Amount = F(TEXT("amount of force"), 0.0f);
			Axis = V(TEXT("twist axis"), FVector3f(0, 0, 1));
		}
		virtual void AddForces(FSourceParticleCollection& P, FSourceParticleContext& Ctx, float Strength, TArray<FVector3f>& Accel) override
		{
			const FSourceParticleCollection* Parent = Ctx.ParentParticles;
			if (Amount == 0.0f || !Parent || Parent->Num() == 0 || Axis.SizeSquared() < 1e-8f)
			{
				return;
			}
			const FVector3f A = Axis.GetSafeNormal();
			const float Force = Amount * Strength;
			const int32 Centres = FMath::Min(Parent->Num(), 16);
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const FVector3f Position = P.GetVector(EAttr::Xyz, i);
				FVector3f Total = FVector3f::ZeroVector;
				for (int32 c = 0; c < Centres; ++c)
				{
					FVector3f Radial = Position - Parent->GetVector(EAttr::Xyz, c);
					Radial -= A * FVector3f::DotProduct(Radial, A);
					const float Length = Radial.Size();
					if (Length < 1e-6f)
					{
						continue;
					}
					Total += FVector3f::CrossProduct(Radial / Length, A) * Force;
				}
				Accel[i] += Total;
			}
		}
	};

	// ---- Constraints --------------------------------------------------------------------------------------------

	/** Constrain distance to control point: a spherical shell around the point. */
	class FConstrainDistanceToControlPoint : public FSourceParticleOp
	{
		float MinDist = 0.0f, MaxDist = 100.0f;
		int32 ControlPoint = 0;
		FVector3f Offset = FVector3f::ZeroVector;
		bool bGlobalCenter = false;
		virtual void Configure() override
		{
			MinDist = F(TEXT("minimum distance"), 0.0f);
			MaxDist = F(TEXT("maximum distance"), 100.0f);
			ControlPoint = I(TEXT("control point number"), 0);
			Offset = V(TEXT("offset of center"), FVector3f::ZeroVector);
			bGlobalCenter = B(TEXT("global center point"), false);
		}
		virtual bool ApplyConstraint(FSourceParticleCollection& P, FSourceParticleContext& Ctx) override
		{
			const FSourceParticleControlPoint& CP = Ctx.CP(ControlPoint);
			const FVector3f Center = bGlobalCenter ? Offset : CP.Position + CP.TransformLocal(Offset);
			bool bChanged = false;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const FVector3f Rel = P.GetVector(EAttr::Xyz, i) - Center;
				const float Distance = Rel.Size();
				const float Clamped = FMath::Clamp(Distance, MinDist, MaxDist);
				if (Clamped == Distance)
				{
					continue;
				}
				const FVector3f Direction = Distance > 1e-6f ? Rel / Distance : FVector3f(0, 0, 1);
				P.SetVector(EAttr::Xyz, i, Center + Direction * Clamped);
				bChanged = true;
			}
			return bChanged;
		}
	};

	/** Prevent passing through a plane. */
	class FPreventPassingThroughPlane : public FSourceParticleOp
	{
		int32 ControlPoint = 0;
		FVector3f PlanePoint = FVector3f::ZeroVector, PlaneNormal = FVector3f(0, 0, 1);
		bool bGlobalOrigin = false, bGlobalNormal = false;
		virtual void Configure() override
		{
			ControlPoint = I(TEXT("control point number"), 0);
			PlanePoint = V(TEXT("plane point"), FVector3f::ZeroVector);
			PlaneNormal = V(TEXT("plane normal"), FVector3f(0, 0, 1));
			bGlobalOrigin = B(TEXT("global origin"), false);
			bGlobalNormal = B(TEXT("global normal"), false);
		}
		virtual bool ApplyConstraint(FSourceParticleCollection& P, FSourceParticleContext& Ctx) override
		{
			const FSourceParticleControlPoint& CP = Ctx.CP(ControlPoint);
			const FVector3f Point = bGlobalOrigin ? PlanePoint : CP.Position + CP.TransformLocal(PlanePoint);
			FVector3f Normal = bGlobalNormal ? PlaneNormal : CP.TransformLocal(PlaneNormal);
			if (Normal.SizeSquared() < 1e-8f)
			{
				return false;
			}
			Normal.Normalize();
			bool bChanged = false;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const FVector3f Xyz = P.GetVector(EAttr::Xyz, i);
				const float Depth = FVector3f::DotProduct(Xyz - Point, Normal) - P.GetFloat(EAttr::Radius, i);
				if (Depth >= 0.0f)
				{
					continue;
				}
				P.SetVector(EAttr::Xyz, i, Xyz - Normal * Depth);
				bChanged = true;
			}
			return bChanged;
		}
	};

	/** Constrain particles to a box. */
	class FConstrainParticlesToBox : public FSourceParticleOp
	{
		FVector3f Min = FVector3f::ZeroVector, Max = FVector3f::ZeroVector;
		virtual void Configure() override
		{
			Min = V(TEXT("min coords"), FVector3f::ZeroVector);
			Max = V(TEXT("max coords"), FVector3f::ZeroVector);
		}
		virtual bool ApplyConstraint(FSourceParticleCollection& P, FSourceParticleContext& Ctx) override
		{
			bool bChanged = false;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const FVector3f Xyz = P.GetVector(EAttr::Xyz, i);
				const FVector3f Clamped = FVector3f::Min(FVector3f::Max(Xyz, Min), Max);
				if (Clamped == Xyz)
				{
					continue;
				}
				P.SetVector(EAttr::Xyz, i, Clamped);
				bChanged = true;
			}
			return bChanged;
		}
	};

	/**
	 * Collision via traces: the world, for real. Each particle's step from PREV_XYZ to XYZ is traced; a hit puts
	 * it on the surface and rebuilds PREV_XYZ so the verlet velocity reflects (bounce) and keeps its tangential
	 * part (slide), or kills it. Source caches traces in a grid to keep this cheap; here each moving particle
	 * traces once per step, which the sizes of Half-Life's effects can afford.
	 */
	class FCollisionViaTraces : public FSourceParticleOp
	{
		float Bounce = 0.0f, Slide = 0.0f, RadiusScale = 1.0f, MinSpeedToKill = -1.0f;
		bool bKillOnCollision = false;
		virtual void Configure() override
		{
			Bounce = F(TEXT("amount of bounce"), 0.0f);
			Slide = F(TEXT("amount of slide"), 0.0f);
			RadiusScale = F(TEXT("radius scale"), 1.0f);
			bKillOnCollision = B(TEXT("kill particle on collision"), false);
			MinSpeedToKill = F(TEXT("minimum speed to kill on collision"), -1.0f);
		}
		virtual bool IsFinalConstraint() const override { return Bounce != 0.0f || Slide != 0.0f; }
		virtual bool ApplyConstraint(FSourceParticleCollection& P, FSourceParticleContext& Ctx) override
		{
			if (!Ctx.TraceLine)
			{
				return false;
			}
			bool bChanged = false;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const FVector3f Xyz = P.GetVector(EAttr::Xyz, i);
				const FVector3f Prev = P.GetVector(EAttr::PrevXyz, i);
				const FVector3f Velocity = Xyz - Prev;
				if (Velocity.SizeSquared() < 1e-8f)
				{
					continue;
				}
				// The trace reaches a radius past the position, so a sprite lands on the surface rather than
				// half inside it.
				const float Radius = FMath::Max(0.0f, P.GetFloat(EAttr::Radius, i) * RadiusScale);
				const FVector3f Dir = Velocity.GetSafeNormal();
				FSourceParticleTraceHit Hit;
				if (!Ctx.TraceLine(Prev, Xyz + Dir * Radius, Hit) || !Hit.bHit)
				{
					continue;
				}
				if (bKillOnCollision)
				{
					P.Kill(i);
					continue;
				}
				if (MinSpeedToKill > 0.0f && Ctx.DeltaTime > 0.0f && Velocity.Size() / Ctx.DeltaTime < MinSpeedToKill)
				{
					P.Kill(i);
					continue;
				}
				const FVector3f N = Hit.Normal.GetSafeNormal();
				const FVector3f Landing = Hit.Position + N * Radius;
				const float NormalSpeed = FVector3f::DotProduct(Velocity, N);
				const FVector3f Tangential = (Velocity - N * NormalSpeed) * Slide;
				const FVector3f Reflected = -N * NormalSpeed * Bounce;
				P.SetVector(EAttr::Xyz, i, Landing);
				P.SetVector(EAttr::PrevXyz, i, Landing - Tangential - Reflected);
				bChanged = true;
			}
			return bChanged;
		}
	};

	/** Prevent passing through static part of world: the same trace, sliding along whatever is hit. */
	class FPreventPassingThroughStaticPartOfWorld : public FSourceParticleOp
	{
		virtual bool IsFinalConstraint() const override { return true; }
		virtual bool ApplyConstraint(FSourceParticleCollection& P, FSourceParticleContext& Ctx) override
		{
			if (!Ctx.TraceLine)
			{
				return false;
			}
			bool bChanged = false;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const FVector3f Xyz = P.GetVector(EAttr::Xyz, i);
				const FVector3f Prev = P.GetVector(EAttr::PrevXyz, i);
				const FVector3f Velocity = Xyz - Prev;
				if (Velocity.SizeSquared() < 1e-8f)
				{
					continue;
				}
				FSourceParticleTraceHit Hit;
				if (!Ctx.TraceLine(Prev, Xyz, Hit) || !Hit.bHit)
				{
					continue;
				}
				const FVector3f N = Hit.Normal.GetSafeNormal();
				const FVector3f Landing = Hit.Position + N * 0.5f;
				const FVector3f Tangential = Velocity - N * FVector3f::DotProduct(Velocity, N);
				P.SetVector(EAttr::Xyz, i, Landing);
				P.SetVector(EAttr::PrevXyz, i, Landing - Tangential);
				bChanged = true;
			}
			return bChanged;
		}
	};

	/** Constrain distance to path between two control points: a tube around the bezier path. */
	class FConstrainDistanceToPathBetweenTwoControlPoints : public FSourceParticleOp
	{
		float MinDist = 0.0f, MaxDist = 100.0f, MidPoint = 0.5f;
		int32 StartCP = 0, EndCP = 0, BulgeControl = 0;
		virtual void Configure() override
		{
			MinDist = F(TEXT("minimum distance"), 0.0f);
			MaxDist = F(TEXT("maximum distance"), 100.0f);
			StartCP = I(TEXT("start control point number"), 0);
			EndCP = I(TEXT("end control point number"), 0);
			BulgeControl = I(TEXT("bulge control 0=random 1=orientation of start pnt 2=orientation of end point"), 0);
			MidPoint = F(TEXT("mid point position"), 0.5f);
		}
		virtual bool ApplyConstraint(FSourceParticleCollection& P, FSourceParticleContext& Ctx) override
		{
			if (MaxDist <= 0.0f)
			{
				return false;
			}
			constexpr int32 Samples = 24;
			FVector3f Path[Samples + 1];
			for (int32 s = 0; s <= Samples; ++s)
			{
				Path[s] = Bezier(Ctx, StartCP, EndCP, MidPoint, 0.0f, BulgeControl, FVector3f::ZeroVector, (float)s / Samples);
			}
			bool bChanged = false;
			for (int32 i = 0; i < P.Num(); ++i)
			{
				const FVector3f Xyz = P.GetVector(EAttr::Xyz, i);
				FVector3f Nearest = Path[0];
				float BestSq = FLT_MAX;
				for (int32 s = 0; s <= Samples; ++s)
				{
					const float DistSq = (Xyz - Path[s]).SizeSquared();
					if (DistSq < BestSq)
					{
						BestSq = DistSq;
						Nearest = Path[s];
					}
				}
				const FVector3f Offset = Xyz - Nearest;
				const float Distance = Offset.Size();
				const float Clamped = FMath::Clamp(Distance, MinDist, MaxDist);
				if (Clamped == Distance)
				{
					continue;
				}
				const FVector3f Direction = Distance > 1e-6f ? Offset / Distance : FVector3f(0, 0, 1);
				P.SetVector(EAttr::Xyz, i, Nearest + Direction * Clamped);
				bChanged = true;
			}
			return bChanged;
		}
	};

	// ---- Renderers and placeholders -----------------------------------------------------------------------------

	/** A renderer's per-step work is nothing; the actor draws from the particle data. Present so it is not "missing". */
	class FRendererOp : public FSourceParticleOp {};

	/** An operator nobody has written yet: keeps its slot in the random-offset sequence and does nothing. */
	class FNoOp : public FSourceParticleOp
	{
		virtual bool IsImplemented() const override { return false; }
	};

	const FSourceParticleOpRegistry& GetRegistry()
	{
		static const FSourceParticleOpRegistry Registry = []()
		{
			FSourceParticleOpRegistry R;
			RegisterSourceParticleEmitterOps(R);
			RegisterSourceParticleInitializerOps(R);
			RegisterSourceParticleOperatorOps(R);
			RegisterSourceParticleForceOps(R);
			return R;
		}();
		return Registry;
	}
}

void RegisterSourceParticleForceOps(FSourceParticleOpRegistry& R)
{
	LAMBDA_PARTICLE_OP(R, "forces", "random force", FRandomForce);
	LAMBDA_PARTICLE_OP(R, "forces", "twist around axis", FTwistAroundAxis);
	LAMBDA_PARTICLE_OP(R, "forces", "Pull towards control point", FPullTowardsControlPoint);
	LAMBDA_PARTICLE_OP(R, "forces", "time varying force", FTimeVaryingForce);
	LAMBDA_PARTICLE_OP(R, "forces", "turbulent force", FTurbulentForce);
	LAMBDA_PARTICLE_OP(R, "forces", "Force based on distance from plane", FForceBasedOnDistanceFromPlane);
	LAMBDA_PARTICLE_OP(R, "forces", "Create vortices from parent particles", FCreateVorticesFromParentParticles);

	LAMBDA_PARTICLE_OP(R, "constraints", "Constrain distance to control point", FConstrainDistanceToControlPoint);
	LAMBDA_PARTICLE_OP(R, "constraints", "Prevent passing through a plane", FPreventPassingThroughPlane);
	LAMBDA_PARTICLE_OP(R, "constraints", "Constrain particles to a box", FConstrainParticlesToBox);
	LAMBDA_PARTICLE_OP(R, "constraints", "Collision via traces", FCollisionViaTraces);
	LAMBDA_PARTICLE_OP(R, "constraints", "Prevent passing through static part of world", FPreventPassingThroughStaticPartOfWorld);
	LAMBDA_PARTICLE_OP(R, "constraints", "Constrain distance to path between two control points", FConstrainDistanceToPathBetweenTwoControlPoints);

	LAMBDA_PARTICLE_OP(R, "renderers", "render_animated_sprites", FRendererOp);
	LAMBDA_PARTICLE_OP(R, "renderers", "render_sprite_trail", FRendererOp);
	LAMBDA_PARTICLE_OP(R, "renderers", "render_rope", FRendererOp);
	LAMBDA_PARTICLE_OP(R, "renderers", "render_points", FRendererOp);
	LAMBDA_PARTICLE_OP(R, "renderers", "render_blobs", FRendererOp);
	LAMBDA_PARTICLE_OP(R, "renderers", "render_screen_velocity_rotate", FRendererOp);
	LAMBDA_PARTICLE_OP(R, "renderers", "Render models", FRendererOp);
	LAMBDA_PARTICLE_OP(R, "renderers", "Render projected", FRendererOp);
}

namespace SourceParticleOps
{
	TUniquePtr<FSourceParticleOp> Create(const TCHAR* Category, const FString& FunctionName, const FSourceDMXElement* Element, int32 InstanceSeed)
	{
		const FSourceParticleOpRegistry& Registry = GetRegistry();
		FSourceParticleOp* Op = nullptr;
		if (const FSourceParticleOpRegistry::FFactory* Factory = Registry.Factories.Find(FSourceParticleOpRegistry::Key(Category, FunctionName)))
		{
			Op = (*Factory)();
		}
		else
		{
			// Said once per name, so a PCF leaning on something unwritten is visible in the log without flooding it.
			static TSet<FString> Reported;
			const FString Key = FSourceParticleOpRegistry::Key(Category, FunctionName);
			if (!Reported.Contains(Key))
			{
				Reported.Add(Key);
				UE_LOG(LogLambdaSource, Log, TEXT("Particle %s '%s' is not implemented; it will do nothing"), Category, *FunctionName);
			}
			Op = new FNoOp();
		}
		Op->Init(Element, InstanceSeed);
		return TUniquePtr<FSourceParticleOp>(Op);
	}

	bool IsImplemented(const TCHAR* Category, const FString& FunctionName)
	{
		return GetRegistry().Factories.Contains(FSourceParticleOpRegistry::Key(Category, FunctionName));
	}
}
