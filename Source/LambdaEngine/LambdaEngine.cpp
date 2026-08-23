#include "LambdaEngine.h"
#include "LambdaFileSystem.h"
#include "Engine/World.h"
#include "GameMapsSettings.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"

DEFINE_LOG_CATEGORY(LogLambda);

void FLambdaEngineModule::StartupModule()
{
	UE_LOG(LogLambda, Log, TEXT("LambdaEngine game module started"));
}

void FLambdaEngineModule::ShutdownModule()
{
}

IMPLEMENT_PRIMARY_GAME_MODULE(FLambdaEngineModule, LambdaEngine, "LambdaEngine");

// ---------------------------------------------------------------------------------------------------------------------
// Console commands
// ---------------------------------------------------------------------------------------------------------------------

static void LambdaMapCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World)
	{
		return;
	}
	if (Args.Num() < 1 || Args[0].IsEmpty())
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: lambda.map <mapname>   (loads <gamedir>/maps/<mapname>.bsp)"));
		return;
	}
	const FString MapName = Args[0];
	const FString EntryMap = UGameMapsSettings::GetGameDefaultMap();
	UE_LOG(LogLambda, Log, TEXT("lambda.map: reloading '%s' with Source map '%s'"), *EntryMap, *MapName);
	UGameplayStatics::OpenLevel(World, FName(*EntryMap), true, FString::Printf(TEXT("map=%s"), *MapName));
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaMapCommand(
	TEXT("lambda.map"),
	TEXT("Load a Source BSP map from the game directory: lambda.map <name>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaMapCommand));

static void LambdaMapsCommand(const TArray<FString>& Args, UWorld* World)
{
	TArray<FString> Maps;
	FLambdaFileSystem::Get().FindFiles(TEXT("maps"), TEXT("*.bsp"), Maps);
	Maps.Sort();
	UE_LOG(LogLambda, Display, TEXT("%d map(s) across mounts: %s"), Maps.Num(), *FString::Join(FLambdaFileSystem::Get().GetMountDescriptions(), TEXT("; ")));
	for (const FString& Map : Maps)
	{
		UE_LOG(LogLambda, Display, TEXT("  %s"), *Map);
	}
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaMapsCommand(
	TEXT("lambda.maps"),
	TEXT("List the BSP maps available in the game directories"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaMapsCommand));
