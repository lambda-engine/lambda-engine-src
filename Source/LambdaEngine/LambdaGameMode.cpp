#include "LambdaGameMode.h"
#include "LambdaMainMenu.h"
#include "Engine/GameInstance.h"
#include "LambdaCharacter.h"
#include "LambdaHUD.h"
#include "LambdaEngine.h"
#include "Core/LambdaSourceSettings.h"
#include "World/SourceBSPWorldActor.h"
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
	// "+map <name>", the way Source's launcher takes it: LambdaEngine.exe +map mymap runs mymap.bsp.
	if (Name.IsEmpty())
	{
		const TCHAR* Cmd = FCommandLine::Get();
		FString Token;
		while (FParse::Token(Cmd, Token, false))
		{
			if (Token.Equals(TEXT("+map"), ESearchCase::IgnoreCase))
			{
				FParse::Token(Cmd, Name, false);
				Name.TrimQuotesInline();
				break;
			}
		}
	}
	// Deliberately no fall back to DefaultMap: with nothing asked for, the game opens its menu, and DefaultMap
	// is what New Game starts there.
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
		// Nothing was asked for, so the game starts where Source starts: at the menu. DefaultMap is what New
		// Game will load from there.
		UE_LOG(LogLambda, Log, TEXT("No map requested - opening the main menu"));
		if (UGameInstance* Instance = World->GetGameInstance())
		{
			if (ULambdaMainMenu* Menu = Instance->GetSubsystem<ULambdaMainMenu>())
			{
				Menu->Show();
			}
		}
		return;
	}

	// A map was asked for, so whatever the menu was doing is over.
	if (UGameInstance* Instance = World->GetGameInstance())
	{
		if (ULambdaMainMenu* Menu = Instance->GetSubsystem<ULambdaMainMenu>())
		{
			Menu->Hide();
		}
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
