#pragma once

#include "CoreMinimal.h"
#include "World/SourceEntity.h"
#include "SourcePointTemplate.generated.h"

/** point_template's spawnflags (point_template.cpp). */
namespace SourcePointTemplateFlags
{
	constexpr int32 DontRemoveTemplateEntities = 0x0001;
	constexpr int32 PreserveNames = 0x0002;
}

/**
 * One entity a template owns: the keyvalues the map wrote for it, and where it sat relative to the template.
 * Source keeps the same pair - the entity's map text in the template store, and matEntityToTemplate beside it -
 * so an instance can be put down anywhere and the group keeps its shape.
 */
struct FSourceTemplateEntry
{
	FSourceEntity Entity;
	FVector3f OriginToTemplate = FVector3f::ZeroVector;
	FVector3f AnglesToTemplate = FVector3f::ZeroVector;
};

/**
 * point_template (game/server/point_template.cpp). Holds the keyvalues of the entities named in Template01..16
 * and stamps out copies of them on demand.
 *
 * The entities it names never live in the map: at load the template takes their keyvalues and they are removed
 * (unless the DontRemoveTemplateEntities spawnflag), so a templated headcrab is not standing there when the map
 * opens - it appears when something fires ForceSpawn.
 */
UCLASS()
class LAMBDASOURCE_API ASourcePointTemplate : public ASourceEntity
{
	GENERATED_BODY()

public:
	virtual void InitializeEntity(const FSourceEntity& InEntity, ASourceBSPWorldActor* InWorldActor) override;
	virtual bool AcceptInput(const FString& InputName, AActor* Activator, AActor* Caller, const FString& Parameter) override;

	/** StartBuildingTemplates: is this one of the names in Template01..Template16? Supports Source's '*' suffix. */
	bool OwnsEntityNamed(const FString& Name) const;
	/** AddTemplate: keep the entity's keyvalues, and its position and angles in the template's own space. */
	void AddTemplate(const FSourceEntity& TemplateEntity);
	/** ShouldRemoveTemplateEntities: the originals are taken out of the map unless the mapper asked otherwise. */
	bool ShouldRemoveTemplateEntities() const;
	/** AllowNameFixup: instances get unique names unless the mapper asked for the originals to be preserved. */
	bool AllowNameFixup() const;

	/** CreateInstance: stamp the whole group down at a position and angle, and return what was made. */
	bool CreateInstance(const FVector3f& Origin, const FVector3f& Angles, TArray<AActor*>& OutSpawned);

	int32 GetNumTemplates() const { return Templates.Num(); }
	const FVector3f& GetTemplateOrigin() const { return TemplateOrigin; }
	const FVector3f& GetTemplateAngles() const { return TemplateAngles; }

private:
	TArray<FString> TemplateNames;
	TArray<FSourceTemplateEntry> Templates;
	FVector3f TemplateOrigin = FVector3f::ZeroVector;
	FVector3f TemplateAngles = FVector3f::ZeroVector;

	/** Templates_StartUniqueInstance: the number that makes each instance's names its own. */
	static int32 UniqueInstanceNumber;
};
