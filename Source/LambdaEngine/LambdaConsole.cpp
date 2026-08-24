#include "LambdaConsole.h"

#include "LambdaEngine.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameMapsSettings.h"
#include "Kismet/GameplayStatics.h"

// Source's console colours, straight out of platform/resource/SourceScheme.res.
FLinearColor ULambdaConsole::TextColour()       { return FLinearColor(216 / 255.0f, 222 / 255.0f, 211 / 255.0f, 1.0f); }	// BaseText
FLinearColor ULambdaConsole::EchoColour()       { return FLinearColor(196 / 255.0f, 181 / 255.0f,  80 / 255.0f, 1.0f); }	// BrightControlText
FLinearColor ULambdaConsole::WarningColour()    { return FLinearColor(1.0f, 0.35f, 0.25f, 1.0f); }
FLinearColor ULambdaConsole::BackgroundColour() { return FLinearColor( 62 / 255.0f,  70 / 255.0f,  55 / 255.0f, 0.93f); }	// WindowBG

/**
 * Takes the keyboard while the console is open.
 *
 * Slate hands an input processor key presses but not typed characters, so the character is taken off the key
 * event where the platform put one, and worked out from the key itself where it did not.
 */
class FLambdaConsoleInput : public IInputProcessor
{
public:
	explicit FLambdaConsoleInput(ULambdaConsole* InConsole) : Console(InConsole) {}

	virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override {}

	virtual bool HandleKeyDownEvent(FSlateApplication&, const FKeyEvent& KeyEvent) override
	{
		ULambdaConsole* C = Console.Get();
		if (!C)
		{
			return false;
		}
		const FKey Key = KeyEvent.GetKey();

		// The key under Escape opens and closes it, as it always has.
		if (Key == EKeys::Tilde)
		{
			C->Toggle();
			return true;
		}
		if (!C->IsOpen())
		{
			return false;
		}

		// From here the console has the keyboard, and nothing it sees reaches the game.
		if (Key == EKeys::Escape)          { C->SetOpen(false); return true; }
		if (Key == EKeys::Enter)           { C->Submit();       return true; }
		if (Key == EKeys::BackSpace)       { C->Backspace();    return true; }
		if (Key == EKeys::Up)              { C->HistoryBack();  return true; }
		if (Key == EKeys::Down)            { C->HistoryForward(); return true; }
		if (Key == EKeys::PageUp)          { C->Scroll(+5);     return true; }
		if (Key == EKeys::PageDown)        { C->Scroll(-5);     return true; }

		const TCHAR Character = CharacterFor(KeyEvent);
		if (Character != 0)
		{
			C->TypeCharacter(Character);
		}
		return true;
	}

	virtual bool HandleKeyUpEvent(FSlateApplication&, const FKeyEvent&) override
	{
		return Console.IsValid() && Console->IsOpen();
	}

	virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication&, const FPointerEvent& WheelEvent, const FPointerEvent*) override
	{
		if (Console.IsValid() && Console->IsOpen())
		{
			Console->Scroll(WheelEvent.GetWheelDelta() > 0 ? 3 : -3);
			return true;
		}
		return false;
	}

private:
	/** What this key press should put in the entry box, or 0 for keys that type nothing. */
	static TCHAR CharacterFor(const FKeyEvent& KeyEvent)
	{
		// The platform usually says, and it has already applied shift and the keyboard layout.
		const TCHAR FromPlatform = (TCHAR)KeyEvent.GetCharacter();
		if (FromPlatform >= 32 && FromPlatform < 127)
		{
			return FromPlatform;
		}

		// It does not always say, so the common keys are worked out from the key itself. A console that can be
		// typed into with a map name is enough for now.
		const FKey Key = KeyEvent.GetKey();
		const bool bShift = KeyEvent.IsShiftDown();
		const FString Name = Key.GetFName().ToString();
		if (Name.Len() == 1)
		{
			const TCHAR C = Name[0];
			if (C >= TEXT('A') && C <= TEXT('Z'))
			{
				return bShift ? C : (TCHAR)(C - TEXT('A') + TEXT('a'));
			}
			if (C >= TEXT('0') && C <= TEXT('9'))
			{
				return C;
			}
		}
		if (Key == EKeys::SpaceBar)     { return TEXT(' '); }
		if (Key == EKeys::Underscore)   { return TEXT('_'); }
		if (Key == EKeys::Hyphen)       { return bShift ? TEXT('_') : TEXT('-'); }
		if (Key == EKeys::Period)       { return TEXT('.'); }
		if (Key == EKeys::Slash)        { return TEXT('/'); }
		if (Key == EKeys::Backslash)    { return TEXT('\\'); }
		if (Key == EKeys::Semicolon)    { return TEXT(';'); }
		if (Key == EKeys::Quote)        { return TEXT('"'); }
		return 0;
	}

	TWeakObjectPtr<ULambdaConsole> Console;
};

void ULambdaConsole::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (FSlateApplication::IsInitialized())
	{
		InputProcessor = MakeShared<FLambdaConsoleInput>(this);
		// Ahead of everything: the console takes the keyboard off the game while it is open.
		FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor, 0);
	}

	ColorPrint(EchoColour(), TEXT("Lambda Engine console. Type 'map <name>' to load a map."));
}

void ULambdaConsole::Deinitialize()
{
	if (InputProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
	}
	InputProcessor.Reset();
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
