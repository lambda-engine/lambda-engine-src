#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "LambdaGameMode.generated.h"

class ASourceBSPWorldActor;

/**
 * Game mode that turns a Source BSP into the playable level. The BSP is chosen from (in order):
 * the "?map=" URL option, "-map=<name>" on the command line, ULambdaSourceSettings::DefaultMap.
 */
UCLASS()
class LAMBDAENGINE_API ALambdaGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ALambdaGameMode();

	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;

	UFUNCTION(BlueprintPure, Category = "Lambda")
	ASourceBSPWorldActor* GetWorldActor() const { return WorldActor; }

	/** Resolves which Source map to load for the given URL options. */
	static FString ResolveRequestedMapName(const FString& Options);

protected:
	void EnsureMapLoaded();

	UPROPERTY(Transient)
	TObjectPtr<ASourceBSPWorldActor> WorldActor;

	bool bMapLoadAttempted = false;
};
