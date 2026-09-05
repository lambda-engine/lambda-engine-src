#pragma once

#include "CoreMinimal.h"
#include "Formats/SourceDMXFile.h"

struct FSourceParticleDefinition;

/**
 * One operator of a definition: its registered function name (legacy spellings already remapped to the current
 * one) and the DMX element carrying its parameters. The element lives as long as the file it came from, which
 * the definition keeps alive.
 */
struct LAMBDASOURCE_API FSourceParticleOperatorDef
{
	FString FunctionName;
	const FSourceDMXElement* Element = nullptr;
};

/** A DmeParticleChild: another definition that plays along with this one, after a delay. */
struct LAMBDASOURCE_API FSourceParticleChildDef
{
	FString Name;
	TSharedPtr<const FSourceParticleDefinition> Definition;
	float Delay = 0.0f;
	bool bEndCapEffect = false;
};

/**
 * A DmeParticleSystemDefinition (CParticleSystemDefinition, public/particles/particles.h), read from a .pcf.
 *
 * Only the fields the simulation and the renderer act on are unpacked into members; everything else stays in
 * the element. The operator lists keep the file's order, which is the order they run in.
 */
struct LAMBDASOURCE_API FSourceParticleDefinition
{
	FString Name;
	FString Material;						// normalised: forward slashes, no "materials/", no ".vmt"
	int32 MaxParticles = 1000;
	int32 InitialParticles = 0;
	FVector3f BoundingBoxMin = FVector3f(-10, -10, -10);
	FVector3f BoundingBoxMax = FVector3f(10, 10, 10);

	// The constant block: what an attribute reads as when nothing writes it per particle.
	FColor Color = FColor::White;
	float Radius = 5.0f;
	float Rotation = 0.0f;					// degrees in the file; radians in the simulation
	float RotationSpeed = 0.0f;
	FVector3f Normal = FVector3f(0, 0, 1);
	int32 SequenceNumber = 0;
	int32 SequenceNumber1 = 0;

	float CullRadius = 0.0f;
	float MaximumDrawDistance = 100000.0f;
	float TimeToSleepWhenNotDrawn = 8.0f;
	bool bSortParticles = true;
	float MaximumTimeStep = 0.1f;
	float MinimumSimulationTimeStep = 0.0f;
	float FreezeSimulationAfterTime = 1000000000.0f;
	bool bViewModelEffect = false;
	bool bScreenSpaceEffect = false;
	int32 GroupId = 0;
	int32 ControlPointToDisableRenderingIfCamera = -1;
	int32 ControlPointToOnlyEnableRenderingIfCamera = -1;

	TArray<FSourceParticleOperatorDef> Renderers;
	TArray<FSourceParticleOperatorDef> Operators;
	TArray<FSourceParticleOperatorDef> Initializers;
	TArray<FSourceParticleOperatorDef> Emitters;
	TArray<FSourceParticleOperatorDef> Forces;
	TArray<FSourceParticleOperatorDef> Constraints;
	TArray<FSourceParticleChildDef> Children;

	/** The file the elements belong to; held so the operator elements above stay valid. */
	TSharedPtr<FSourceDMXFile> File;
	/** Where it was read from ("particles/explosion.pcf"), for messages. */
	FString SourceFile;

	/**
	 * RemapOperatorName (particles/particles.cpp): the old spellings a functionName may still carry, turned into
	 * the registered name. Unknown names pass through unchanged; matching is case-insensitive either way.
	 */
	static FString CanonicalOperatorName(const FString& FunctionName);
};
