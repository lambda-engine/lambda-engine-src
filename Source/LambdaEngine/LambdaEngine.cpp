#include "LambdaEngine.h"
#include "LambdaFileSystem.h"
#include "Engine/World.h"
#include "GameMapsSettings.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "LambdaSourceSettings.h"
#include "SourceCoordinates.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

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

// Source's setpos/setang, handy for testing maps. Coordinates are Hammer units, like the originals.
static void LambdaSetPosCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 3)
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: lambda.setpos <x> <y> <z>   (Hammer units)"));
		return;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		return;
	}
	const FVector3f Source(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]), FCString::Atof(*Args[2]));
	const float Scale = ULambdaSourceSettings::Get().UnitScale;
	const FVector Target = FSourceCoords::ToUE(Source, Scale);
	Pawn->TeleportTo(Target, Pawn->GetActorRotation());
	UE_LOG(LogLambda, Display, TEXT("setpos Source(%s) -> UE(%s)"), *Source.ToString(), *Target.ToString());
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaSetPosCommand(
	TEXT("lambda.setpos"),
	TEXT("Teleport the player to a position in Hammer units: lambda.setpos <x> <y> <z>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaSetPosCommand));

static void LambdaSetAngCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 2)
	{
		UE_LOG(LogLambda, Display, TEXT("Usage: lambda.setang <pitch> <yaw> [roll]   (Source angles)"));
		return;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC)
	{
		return;
	}
	const FVector3f Angles(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]), Args.Num() > 2 ? FCString::Atof(*Args[2]) : 0.0f);
	PC->SetControlRotation(FSourceCoords::AnglesToUE(Angles));
	UE_LOG(LogLambda, Display, TEXT("setang Source(%s)"), *Angles.ToString());
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaSetAngCommand(
	TEXT("lambda.setang"),
	TEXT("Set the player's view angles in Source (pitch yaw roll) degrees"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaSetAngCommand));
