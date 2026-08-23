#include "LambdaGameMode.h"
#include "LambdaCharacter.h"
#include "LambdaHUD.h"
#include "LambdaEngine.h"
#include "LambdaSourceSettings.h"
#include "SourceBSPWorldActor.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

ALambdaGameMode::ALambdaGameMode()
{
	DefaultPawnClass = ALambdaCharacter::StaticClass();
	HUDClass = ALambdaHUD::StaticClass();
}

FString ALambdaGameMode::ResolveRequestedMapName(const FString& Options)
{
	// ?map=<name> as a travel URL option (used by the lambda.map console command / OpenLevel).
	FString Name = UGameplayStatics::ParseOption(Options, TEXT("map"));
	// -sourcemap=<name> on the command line. NB: we deliberately do NOT use "-map=", because the engine's own
	// launcher consumes that as the startup level URL and would try to load a UE package of that name.
	if (Name.IsEmpty())
	{
		FParse::Value(FCommandLine::Get(), TEXT("sourcemap="), Name);
		Name.TrimQuotesInline();
	}
	if (Name.IsEmpty())
	{
		Name = ULambdaSourceSettings::Get().DefaultMap;
	}
	Name.TrimStartAndEndInline();
	return Name;
}

void ALambdaGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);
	EnsureMapLoaded();
}

void ALambdaGameMode::EnsureMapLoaded()
{
	if (bMapLoadAttempted)
	{
		return;
	}
	bMapLoadAttempted = true;

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FString SourceMap = ResolveRequestedMapName(OptionsString);
	if (SourceMap.IsEmpty())
	{
		UE_LOG(LogLambda, Warning, TEXT("No Source map requested (use -map=<name>, ?map=<name> or set DefaultMap in Project Settings > Lambda Source)"));
		return;
	}

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	WorldActor = World->SpawnActor<ASourceBSPWorldActor>(ASourceBSPWorldActor::StaticClass(), FTransform::Identity, Params);
	if (!WorldActor)
	{
		UE_LOG(LogLambda, Error, TEXT("Could not spawn the BSP world actor"));
		return;
	}

	UE_LOG(LogLambda, Log, TEXT("Loading Source map '%s'..."), *SourceMap);
	if (!WorldActor->LoadMap(SourceMap))
	{
		UE_LOG(LogLambda, Error, TEXT("Failed to load Source map '%s' - see LogLambdaSource for details"), *SourceMap);
	}
}

AActor* ALambdaGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	EnsureMapLoaded();
	return Super::ChoosePlayerStart_Implementation(Player);
}
