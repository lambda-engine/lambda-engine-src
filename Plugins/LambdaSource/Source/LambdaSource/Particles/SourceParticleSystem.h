#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Particles/SourceParticleSimulation.h"
#include "SourceParticleSystem.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;
class ULambdaMaterialLibrary;
struct FSourceSpriteSheet;

/**
 * A Source particle system playing in the world: CNewParticleEffect, with CParticleCollection::Render behind it.
 *
 * The definition comes from a .pcf through FSourceParticleLibrary; the simulation is FSourceParticleSimulator,
 * run in Source units with control point 0 at the actor; and every frame the live particles of every system in
 * the tree are turned into camera-facing quads (or velocity trails, or a rope) in one procedural mesh, a
 * section per system, with the sprite material the definition names. Sheet animation reads the frames out of
 * the texture's own sprite sheet, so a smoke puff picks the puff Valve drew for that sequence.
 *
 * Create() is DispatchParticleEffect: a one-shot effect that removes itself once every emitter has finished and
 * the last particle has died. env_particles keeps one around and starts and stops it.
 */
UCLASS()
class LAMBDASOURCE_API ASourceParticleSystem : public AActor
{
	GENERATED_BODY()

public:
	ASourceParticleSystem();

	/**
	 * DispatchParticleEffect: an effect at a place, facing a way (Source angles: pitch yaw roll). A one-shot
	 * effect destroys itself when it is done; otherwise the caller owns it. Null when the effect is unknown.
	 */
	static ASourceParticleSystem* Create(UWorld* World, const FString& EffectName, const FVector& Location,
		const FVector3f& SourceAngles, ULambdaMaterialLibrary* Materials, bool bOneShot = true);

	/** Loads the definition and builds the simulation; false (and nothing drawn) when the name is unknown. */
	bool SetEffect(const FString& EffectName, ULambdaMaterialLibrary* Materials);
	/** Starts, or restarts from the beginning. */
	void StartEffect();
	/** StopEmission: nothing new is emitted; what is alive fades on its own terms. */
	void StopEffect(bool bPlayEndCap = false);
	/** Everything gone at once, and the actor with it. */
	void StopAndDestroy();

	/** Control points, in UE space; 0 is the actor itself and follows it. */
	void SetControlPointLocation(int32 Index, const FVector& Location);
	void SetControlPointAngles(int32 Index, const FVector3f& SourceAngles);

	bool IsActive() const { return bActive; }
	/** True once no emitter has anything left to give and no particle is alive anywhere in the tree. */
	bool IsFinished() const;
	const FString& GetEffectName() const { return EffectName; }
	int32 GetTotalParticleCount() const;
	/** One line per system in the tree: name, time, live particles, what was drawn. For particle_stats. */
	FString GetDebugString() const;

	virtual void Tick(float DeltaSeconds) override;

private:
	/** How one system of the tree is drawn: its material, its sheet, and what its renderer asked for. */
	struct FSystemRender
	{
		enum class EKind : uint8 { Sprites, Trail, Rope };

		FSourceParticleSimulator* Simulator = nullptr;
		int32 Section = 0;
		int32 MaterialIndex = INDEX_NONE;		// into SectionMaterials
		bool bAdditive = false;
		// "$dualsequence 1" materials: each quad also carries the second sequence's frame in UV2, and the dual
		// sprite master combines the two samples the way $sequence_blend_mode says.
		bool bDualSequence = false;
		float SecondSequenceRate = 0.0f;
		// The material's screen-size fade (see FSourceMaterialInfo).
		float StartFadeSize = 0.0f;
		float EndFadeSize = 0.0f;
		TSharedPtr<const FSourceSpriteSheet> Sheet;
		int32 TextureHeight = 64;

		EKind Kind = EKind::Sprites;
		// render_animated_sprites
		float AnimationRate = 0.1f;
		bool bFitLifetime = false;
		bool bRateIsFPS = false;
		int32 OrientationType = 0;
		int32 OrientationControlPoint = -1;
		// render_sprite_trail
		float LengthFadeInTime = 0.0f;
		float MaxLength = 2000.0f;
		float MinLength = 0.0f;
		bool bConstrainRadiusToLength = true;
		FVector4f TailScale = FVector4f(1, 1, 1, 1);
		// render_rope
		float TexelSize = 4.0f;
		float TextureScrollRate = 0.0f;
		float TextureOffset = 0.0f;
	};

	/** The quad builder's view of the camera and the unit scale, for one frame. */
	struct FFrameView
	{
		FVector CameraLocation = FVector::ZeroVector;
		FVector CameraRight = FVector::RightVector;
		FVector CameraUp = FVector::UpVector;
		FVector CameraForward = FVector::ForwardVector;
		FVector Origin = FVector::ZeroVector;	// the actor, which the mesh is relative to
		float Scale = 1.0f;						// cm per unit
		float TanHalfFOV = 1.0f;				// for a sprite's size on screen
	};

	/** The four UV corners of a sheet frame, and the next frame with the blend towards it. */
	struct FFrame
	{
		FVector4f UV = FVector4f(0, 0, 1, 1);
		FVector4f NextUV = FVector4f(0, 0, 1, 1);
		float Blend = 0.0f;
	};

	/** Scratch geometry for one section, reused frame to frame. */
	struct FSectionGeometry
	{
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UV0;
		TArray<FVector2D> UV1;
		TArray<FVector2D> UV2;
		TArray<FLinearColor> Colors;
		TArray<struct FProcMeshTangent> Tangents;
		void Reset();
		void AddQuad(const FVector Corners[4], const FVector4f& UV, const FVector4f& UV2Rect, const FLinearColor Colors4[4], float Alpha4[4],
			const FVector& Normal, const FVector& Tangent);
	};

	void BuildRenderList();
	void ApplyWorldHooks();
	void RebuildMesh();
	void FillSprites(const FSystemRender& System, const FFrameView& View, FSectionGeometry& Geometry);
	void FillTrail(const FSystemRender& System, const FFrameView& View, FSectionGeometry& Geometry);
	void FillRope(const FSystemRender& System, const FFrameView& View, FSectionGeometry& Geometry);
	FFrame GetFrame(const FSystemRender& System, int32 Sequence, float Age, float Life, float Rate) const;
	/** Vertex colour and alpha the sprite master wants: additive folds the alpha into the colour. */
	static void PackColor(bool bAdditive, const FVector3f& Tint, float Alpha, FLinearColor& OutColor, float& OutAlpha);

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UProceduralMeshComponent> Mesh;

	UPROPERTY(Transient)
	TObjectPtr<ULambdaMaterialLibrary> MaterialLibrary;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> SectionMaterials;

	TSharedPtr<const FSourceParticleDefinition> Definition;
	TUniquePtr<FSourceParticleSimulator> Root;
	TArray<FSystemRender> Systems;
	TArray<FSectionGeometry> Geometries;
	TArray<int32> SortScratch;
	TArray<float> SortKeys;

	FString EffectName;
	float DebugLogTimer = 0.0f;
	bool bOneShot = false;
	bool bActive = false;
	float EmittersEndTime = 0.0f;
};
