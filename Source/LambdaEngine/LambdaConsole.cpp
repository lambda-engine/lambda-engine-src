#include "LambdaConsole.h"

#include "LambdaEngine.h"
#include "LambdaLoadingScreen.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "GameMapsSettings.h"
#include "Kismet/GameplayStatics.h"

// Source's console colours, straight out of platform/resource/SourceScheme.res.
FLinearColor ULambdaConsole::TextColour()       { return FLinearColor(216 / 255.0f, 222 / 255.0f, 211 / 255.0f, 1.0f); }	// BaseText
FLinearColor ULambdaConsole::EchoColour()       { return FLinearColor(196 / 255.0f, 181 / 255.0f,  80 / 255.0f, 1.0f); }	// BrightControlText
FLinearColor ULambdaConsole::WarningColour()    { return FLinearColor(1.0f, 0.35f, 0.25f, 1.0f); }
FLinearColor ULambdaConsole::BackgroundColour() { return FLinearColor( 62 / 255.0f,  70 / 255.0f,  55 / 255.0f, 0.93f); }	// WindowBG

void ULambdaConsole::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	ColorPrint(EchoColour(), TEXT("Lambda Engine console. Type 'map <name>' to load a map."));
}

void ULambdaConsole::Deinitialize()
{
	Super::Deinitialize();
}

void ULambdaConsole::SetOpen(bool bInOpen)
{
	if (bOpen == bInOpen)
	{
		return;
	}
	bOpen = bInOpen;
	ScrollBack = 0;

	// While it is open the mouse belongs to whoever is behind it, and the game should not be taking fire orders
	// from the keys being typed.
	if (UGameInstance* Instance = GetGameInstance())
	{
		if (APlayerController* PC = Instance->GetFirstLocalPlayerController())
		{
			PC->SetIgnoreLookInput(bOpen);
			PC->SetIgnoreMoveInput(bOpen);
		}
	}
}

void ULambdaConsole::Print(const FString& Text)
{
	ColorPrint(TextColour(), Text);
}

void ULambdaConsole::ColorPrint(const FLinearColor& Color, const FString& Text)
{
	TArray<FString> Split;
	Text.ParseIntoArray(Split, TEXT("\n"), false);
	for (const FString& Line : Split)
	{
		Lines.Add({ Line, Color });
	}
	while (Lines.Num() > MaxLines)
	{
		Lines.RemoveAt(0);
	}
}

void ULambdaConsole::TypeCharacter(TCHAR Character)
{
	Input.AppendChar(Character);
}

void ULambdaConsole::Backspace()
{
	if (Input.Len() > 0)
	{
		Input.LeftChopInline(1);
	}
}

void ULambdaConsole::ClearInput()
{
	Input.Reset();
}

void ULambdaConsole::Submit()
{
	const FString CommandLine = Input.TrimStartAndEnd();
	Input.Reset();
	ScrollBack = 0;

	// CConsolePanel::OnCommand: the command is echoed back with a "] " in front of it, then run.
	ColorPrint(EchoColour(), FString::Printf(TEXT("] %s"), *CommandLine));
	if (CommandLine.IsEmpty())
	{
		return;
	}
	if (History.Num() == 0 || History.Last() != CommandLine)
	{
		History.Add(CommandLine);
	}
	HistoryPosition = History.Num();

	Execute(CommandLine);
}

void ULambdaConsole::HistoryBack()
{
	if (History.Num() == 0 || HistoryPosition == 0)
	{
		return;
	}
	--HistoryPosition;
	Input = History[HistoryPosition];
}

void ULambdaConsole::HistoryForward()
{
	if (HistoryPosition >= History.Num())
	{
		return;
	}
	++HistoryPosition;
	Input = History.IsValidIndex(HistoryPosition) ? History[HistoryPosition] : FString();
}

void ULambdaConsole::Scroll(int32 InLines)
{
	ScrollBack = FMath::Clamp(ScrollBack + InLines, 0, FMath::Max(0, Lines.Num() - 1));
}

void ULambdaConsole::Execute(const FString& CommandLine)
{
	FString Command = CommandLine;
	FString Arguments;
	CommandLine.Split(TEXT(" "), &Command, &Arguments);
	Command.TrimStartAndEndInline();
	Arguments.TrimStartAndEndInline();

	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;

	// map <name>: what Source's map command does - leave whatever is running and start that one.
	if (Command.Equals(TEXT("map"), ESearchCase::IgnoreCase))
	{
		FString MapName = Arguments;
		MapName.TrimQuotesInline();
		if (MapName.IsEmpty())
		{
			ColorPrint(WarningColour(), TEXT("Usage: map <mapname>"));
			return;
		}
		if (!World)
		{
			ColorPrint(WarningColour(), TEXT("map: no world to load into"));
			return;
		}
		Print(FString::Printf(TEXT("Loading map '%s'..."), *MapName));
		SetOpen(false);
		const FString EntryMap = UGameMapsSettings::GetGameDefaultMap();
		FLambdaLoadingScreen::Arm();
		UGameplayStatics::OpenLevel(World, FName(*EntryMap), true, FString::Printf(TEXT("map=%s"), *MapName));
		return;
	}

	// Anything else is handed to the engine's own console, which is where every lambda.* command already lives.
	if (World && GEngine && GEngine->Exec(World, *CommandLine))
	{
		return;
	}
	ColorPrint(WarningColour(), FString::Printf(TEXT("Unknown command: %s"), *Command));
}

// ---------------------------------------------------------------------------------------------------------
// A way in that does not need the key, for when something else has the keyboard - and for tests, which are not
// allowed to press keys.
static ULambdaConsole* FindConsole(UWorld* World)
{
	return World && World->GetGameInstance() ? World->GetGameInstance()->GetSubsystem<ULambdaConsole>() : nullptr;
}

static void LambdaConsoleToggle(const TArray<FString>& Args, UWorld* World)
{
	if (ULambdaConsole* Console = FindConsole(World))
	{
		Console->Toggle();
		UE_LOG(LogLambda, Display, TEXT("console %s"), Console->IsOpen() ? TEXT("open") : TEXT("closed"));
	}
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaConsoleToggle(
	TEXT("lambda.console"),
	TEXT("Open or close the developer console"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaConsoleToggle));

static void LambdaConsoleExec(const TArray<FString>& Args, UWorld* World)
{
	ULambdaConsole* Console = FindConsole(World);
	if (!Console || Args.Num() == 0)
	{
		return;
	}
	// Put it through the entry box exactly as typing it would.
	Console->SetOpen(true);
	for (const FString& Word : Args)
	{
		for (int32 i = 0; i < Word.Len(); ++i)
		{
			Console->TypeCharacter(Word[i]);
		}
		if (&Word != &Args.Last())
		{
			Console->TypeCharacter(TEXT(' '));
		}
	}
	Console->Submit();
}

static FAutoConsoleCommandWithWorldAndArgs GLambdaConsoleExec(
	TEXT("lambda.console.exec"),
	TEXT("Type a line into the developer console and submit it: lambda.console.exec map startup"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LambdaConsoleExec));
