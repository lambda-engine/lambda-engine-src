#pragma once

#include "CoreMinimal.h"
#include "Particles/SourceParticleSimulation.h"

/**
 * Shared by the operator implementation files: the registry they add themselves to and the helpers several of
 * them lean on. Nothing outside Particles/ includes this.
 */
struct FSourceParticleOpRegistry
{
	using FFactory = TFunction<FSourceParticleOp*()>;
	TMap<FString, FFactory> Factories;

	static FString Key(const TCHAR* Category, const FString& Name)
	{
		return FString(Category).ToLower() + TEXT("|") + Name.ToLower();
	}
	void Add(const TCHAR* Category, const TCHAR* Name, FFactory Factory)
	{
		Factories.Add(Key(Category, Name), MoveTemp(Factory));
	}
};

#define LAMBDA_PARTICLE_OP(Registry, Category, Name, Class) \
	(Registry).Add(TEXT(Category), TEXT(Name), []() -> FSourceParticleOp* { return new Class(); })

void RegisterSourceParticleEmitterOps(FSourceParticleOpRegistry& Registry);
void RegisterSourceParticleInitializerOps(FSourceParticleOpRegistry& Registry);
void RegisterSourceParticleOperatorOps(FSourceParticleOpRegistry& Registry);
void RegisterSourceParticleForceOps(FSourceParticleOpRegistry& Registry);

namespace SourceParticleOpHelpers
{
	using namespace SourceParticleMath;

	/**
	 * CParticleCollection::CalculatePathValues: a quadratic bezier from the start control point to the end one
	 * through a midpoint pushed out by the bulge - along the chosen point's forward, or a random direction.
	 */
	inline FVector3f Bezier(const FSourceParticleContext& Ctx, int32 StartCP, int32 EndCP, float MidPointPosition,
		float Bulge, int32 BulgeControl, const FVector3f& RandomBulgeDir, float T)
	{
		const FSourceParticleControlPoint& Start = Ctx.CP(StartCP);
		const FSourceParticleControlPoint& End = Ctx.CP(EndCP);
		FVector3f Mid = FMath::Lerp(Start.Position, End.Position, MidPointPosition);
		if (Bulge != 0.0f)
		{
			const FVector3f Dir = BulgeControl == 1 ? Start.Forward : (BulgeControl == 2 ? End.Forward : RandomBulgeDir);
			if (Dir.SizeSquared() > 1e-8f)
			{
				Mid += Dir.GetSafeNormal() * Bulge;
			}
		}
		const float U = 1.0f - T;
		return U * U * Start.Position + 2.0f * U * T * Mid + T * T * End.Position;
	}

	/** The direction from a point to a control point, optionally normalised, rotated by an offset, scaled. */
	inline FVector3f DirectionToCP(const FVector3f& CPPosition, const FVector3f& From, bool bNormalize,
		const FVector3f& OffsetAxis, float OffsetRotationDegrees, float Scale)
	{
		FVector3f Dir = CPPosition - From;
		if (bNormalize)
		{
			Dir = Dir.GetSafeNormal();
		}
		if (OffsetRotationDegrees != 0.0f && OffsetAxis.SizeSquared() > 1e-8f)
		{
			Dir = RotateAboutAxis(Dir, OffsetAxis.GetSafeNormal(), OffsetRotationDegrees * DegToRad);
		}
		return Dir * Scale;
	}

	/** Writes one component of a control point's position (the "output field 0-2" of the remap-to-CP family). */
	inline void SetCPField(FSourceParticleContext& Ctx, int32 CPIndex, int32 Field, float Value)
	{
		if (CPIndex < 0 || CPIndex >= FSourceParticleContext::MaxControlPoints)
		{
			return;
		}
		FVector3f& P = Ctx.ControlPoints[CPIndex].Position;
		if (Field == 1) { P.Y = Value; }
		else if (Field == 2) { P.Z = Value; }
		else { P.X = Value; }
	}

	inline float Component(const FVector3f& V, int32 Axis)
	{
		return Axis == 1 ? V.Y : (Axis == 2 ? V.Z : V.X);
	}
}
