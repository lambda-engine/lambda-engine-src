#pragma once

#include "CoreMinimal.h"

/**
 * Conversion between Source Engine space and Unreal space.
 *
 *   Source: X forward, Y left,  Z up, right-handed, Hammer units (16 units = 1 foot => 1.905 cm/unit by default)
 *   Unreal: X forward, Y right, Z up, left-handed,  centimetres
 *
 * So: UE = (x, -y, z) * UnitScale. The Y mirror flips handedness, which reverses triangle winding (handled by the
 * geometry builder) and negates yaw.
 */
struct LAMBDASOURCE_API FSourceCoords
{
	/** Centimetres per Hammer unit (ULambdaSourceSettings::UnitScale). */
	static float GetUnitScale();

	static FORCEINLINE FVector ToUE(const FVector3f& P, float Scale)
	{
		return FVector(P.X * Scale, -P.Y * Scale, P.Z * Scale);
	}
	static FORCEINLINE FVector ToUE(const FVector3f& P)
	{
		return ToUE(P, GetUnitScale());
	}
	/** Direction/normal conversion (mirror only, normalised, no scale). */
	static FORCEINLINE FVector ToUEDirection(const FVector3f& D)
	{
		return FVector(D.X, -D.Y, D.Z).GetSafeNormal();
	}
	static FORCEINLINE FVector3f ToSource(const FVector& P, float Scale)
	{
		return FVector3f((float)(P.X / Scale), (float)(-P.Y / Scale), (float)(P.Z / Scale));
	}

	/**
	 * "angles" keyvalue (pitch yaw roll) -> UE rotator for regular entities.
	 * Source +pitch looks down (AngleVectors: forward.z = -sin(pitch)), UE +pitch looks up; yaw and roll flip with the mirror.
	 */
	static FORCEINLINE FRotator AnglesToUE(const FVector3f& Angles)
	{
		return FRotator(-Angles.X, -Angles.Y, -Angles.Z);
	}

	/**
	 * Light entities (light_spot, light_environment) use vrad's convention: normal.z = +sin(pitch), so pitch -90 = straight
	 * down. That matches UE's pitch sign directly; only yaw is mirrored.
	 */
	static FORCEINLINE FRotator LightAnglesToUE(float Pitch, float Yaw)
	{
		return FRotator(Pitch, -Yaw, 0.0f);
	}

	/** Parses "x y z" (also tolerates commas and extra whitespace). */
	static bool ParseVector(const FString& Text, FVector3f& Out);
	/** Parses a whitespace separated list of numbers. */
	static bool ParseFloats(const FString& Text, TArray<float>& Out);
};
