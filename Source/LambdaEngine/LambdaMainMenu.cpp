#include "LambdaMainMenu.h"

#include "LambdaConsole.h"
#include "LambdaEngine.h"
#include "FileSystem/LambdaFileSystem.h"
#include "LambdaLoadingScreen.h"
#include "Core/LambdaSourceSettings.h"
#include "Formats/SourceKeyValues.h"

#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "GameMapsSettings.h"
#include "Kismet/GameplayStatics.h"

// SourceScheme.res again: the menu is written in the same off-white, and whatever the mouse is over goes gold.
// The game's name is plain white, and stands apart from the items by its face and its size rather than its colour.
FLinearColor ULambdaMainMenu::ItemColour()     { return FLinearColor(216 / 255.0f, 222 / 255.0f, 211 / 255.0f, 1.0f); }	// BaseText
FLinearColor ULambdaMainMenu::SelectedColour() { return FLinearColor(196 / 255.0f, 181 / 255.0f,  80 / 255.0f, 1.0f); }	// BrightControlText
FLinearColor ULambdaMainMenu::TitleColour()    { return FLinearColor::White; }

void ULambdaMainMenu::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void ULambdaMainMenu::Deinitialize()
{
	Super::Deinitialize();
}

void ULambdaMainMenu::Show()
{
	bPauseMenu = false;
	LoadItems(/*bInGame=*/ false);
	Selected = 0;
	bActive = true;
	TickInputState();
	UE_LOG(LogLambda, Log, TEXT("Main menu: %d items"), Items.Num());
}

void ULambdaMainMenu::ShowPauseMenu()
{
	bPauseMenu = true;
	LoadItems(/*bInGame=*/ true);
	Selected = 0;
	bActive = true;
	SetPaused(true);
	TickInputState();
	UE_LOG(LogLambda, Log, TEXT("Pause menu: %d items"), Items.Num());
}

void ULambdaMainMenu::Hide()
{
	const bool bWasPaused = bPauseMenu;
	bActive = false;
	bPauseMenu = false;
	if (bWasPaused)
	{
		SetPaused(false);
	}
	TickInputState();
}

void ULambdaMainMenu::SetPaused(bool bPaused)
{
	if (UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr)
	{
		UGameplayStatics::SetGamePaused(World, bPaused);
	}
}

void ULambdaMainMenu::TickInputState()
{
	UGameInstance* Instance = GetGameInstance();
	APlayerController* PC = Instance ? Instance->GetFirstLocalPlayerController() : nullptr;
	if (!PC)
	{
		// The menu can open before there is a controller to show a cursor on; the HUD calls back every frame
		// until there is one.
		return;
	}
	if (bInputStateApplied == bActive && PC == LastController.Get())
	{
		return;
	}
	bInputStateApplied = bActive;
	LastController = PC;

	PC->bShowMouseCursor = bActive;
	if (bActive)
	{
		// The cursor alone is not enough: without this the game still swallows the mouse and nothing can be
		// clicked.
		FInputModeGameAndUI Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(false);
		PC->SetInputMode(Mode);
		PC->SetIgnoreLookInput(true);
		PC->SetIgnoreMoveInput(true);
	}
	else
	{
		PC->SetInputMode(FInputModeGameOnly());
		// Cleared rather than decremented, so the player gets his hands back whatever happened while it was up.
		PC->ResetIgnoreInputFlags();
	}
}

void ULambdaMainMenu::MoveSelection(int32 Delta)
{
	if (Items.Num() == 0)
	{
		return;
	}
	Selected = (Selected + Delta + Items.Num()) % Items.Num();
}

void ULambdaMainMenu::SetSelected(int32 Index)
{
	if (Items.IsValidIndex(Index))
	{
		Selected = Index;
	}
}

bool ULambdaMainMenu::SelectAt(const FVector2D& Position)
{
	for (int32 i = 0; i < Items.Num(); ++i)
	{
		// The bounds are whatever the last frame drew, so what is clicked is what was seen.
		if (Items[i].Bounds.bIsValid && Items[i].Bounds.IsInside(Position))
		{
			Selected = i;
			return true;
		}
	}
	return false;
}

void ULambdaMainMenu::Activate()
{
	if (Items.IsValidIndex(Selected))
	{
		RunCommand(Items[Selected].Command);
	}
}

void ULambdaMainMenu::RunCommand(const FString& Command)
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	ULambdaConsole* Console = GetGameInstance() ? GetGameInstance()->GetSubsystem<ULambdaConsole>() : nullptr;

	// OpenNewGameDialog: there is no chapter list yet, so New Game starts the map the game is configured to
	// start on - what DefaultMap has always meant.
	if (Command.Equals(TEXT("OpenNewGameDialog"), ESearchCase::IgnoreCase))
	{
		const FString Map = ULambdaSourceSettings::Get().DefaultMap;
		UE_LOG(LogLambda, Log, TEXT("Main menu: new game on '%s'"), *Map);
		Hide();
		if (World)
		{
			FLambdaLoadingScreen::Arm();
			const FString EntryMap = UGameMapsSettings::GetGameDefaultMap();
			UGameplayStatics::OpenLevel(World, FName(*EntryMap), true, FString::Printf(TEXT("map=%s"), *Map));
		}
		return;
	}
	if (Command.Equals(TEXT("Quit"), ESearchCase::IgnoreCase))
	{
		if (World)
		{
			UKismetSystemLibrary::QuitGame(World, World->GetFirstPlayerController(), EQuitPreference::Quit, false);
		}
		return;
	}
	if (Command.Equals(TEXT("ResumeGame"), ESearchCase::IgnoreCase))
	{
		Hide();
		return;
	}

	// Everything else is a dialog that does not exist yet. Say so rather than doing nothing.
	UE_LOG(LogLambda, Log, TEXT("Main menu: '%s' is not implemented yet"), *Command);
	if (Console)
	{
		Console->ColorPrint(ULambdaConsole::WarningColour(),
			FString::Printf(TEXT("Main menu: '%s' is not implemented yet"), *Command));
	}
}

void ULambdaMainMenu::LoadItems(bool bInGame)
{
	Items.Reset();

	// GameMenu.res: the menu as the game directory describes it.
	TArray<uint8> Bytes;
	FSourceKeyValues Root;
	const bool bLoaded = FLambdaFileSystem::Get().ReadFile(TEXT("resource/gamemenu.res"), Bytes)
		&& FSourceKeyValues::ParseSingle(Bytes, Root, nullptr);

	if (bLoaded)
	{
		for (const FSourceKeyValues& Entry : Root.Children)
		{
			if (!Entry.IsSection())
			{
				continue;
			}
			FLambdaMenuItem Item;
			Item.Label = ResolveLabel(Entry.GetString(TEXT("label")));
			Item.Command = Entry.GetString(TEXT("command"));
			Item.bOnlyInGame = Entry.GetString(TEXT("OnlyInGame")) == TEXT("1");
			Item.bNotMulti = Entry.GetString(TEXT("notmulti")) == TEXT("1");
			Item.InGameOrder = FCString::Atoi(*Entry.GetString(TEXT("InGameOrder")));

			// Not in a game, so the ones that only make sense in one are left out - and the console-only and
			// VR entries with them, which this engine has nothing to say about.
			const bool bConsoleOnly = Entry.GetString(TEXT("ConsoleOnly")) == TEXT("1");
			const bool bVR = !Entry.GetString(TEXT("OnlyWhenVREnabled")).IsEmpty();
			if ((Item.bOnlyInGame && !bInGame) || bConsoleOnly || bVR || Item.Command.IsEmpty() || Item.Label.IsEmpty())
			{
				continue;
			}
			Items.Add(MoveTemp(Item));
		}
	}

	// In a game the file says what order to put them in, and Resume belongs at the top.
	if (bInGame)
	{
		Items.Sort([](const FLambdaMenuItem& A, const FLambdaMenuItem& B) { return A.InGameOrder < B.InGameOrder; });
	}

	if (Items.Num() == 0)
	{
		// Nothing to read, so the menu is the one Half-Life 2 has.
		if (bInGame)
		{
			Items.Add({ TEXT("RESUME GAME"), TEXT("ResumeGame") });
		}
		Items.Add({ TEXT("NEW GAME"), TEXT("OpenNewGameDialog") });
		Items.Add({ TEXT("LOAD GAME"), TEXT("OpenLoadGameDialog") });
		Items.Add({ TEXT("OPTIONS"), TEXT("OpenOptionsDialog") });
		Items.Add({ TEXT("QUIT"), TEXT("Quit") });
	}
}

FString ULambdaMainMenu::ResolveLabel(const FString& Label)
{
	if (!Label.StartsWith(TEXT("#")))
	{
		return Label;
	}
	const FString Token = Label.RightChop(1);

	// resource/gameui_english.txt, where "#GameUI_GameMenu_NewGame" turns into "NEW GAME". It is UTF-16, so it
	// is read as text and handed to the parser as bytes it understands.
	static TMap<FString, FString> Tokens;
	static bool bLoaded = false;
	if (!bLoaded)
	{
		bLoaded = true;
		TArray<uint8> Bytes;
		if (FLambdaFileSystem::Get().ReadFile(TEXT("resource/gameui_english.txt"), Bytes) && Bytes.Num() > 2)
		{
			FString Text;
			if (Bytes[0] == 0xFF && Bytes[1] == 0xFE)
			{
				const int32 NumChars = (Bytes.Num() - 2) / 2;
				const UTF16CHAR* Wide = reinterpret_cast<const UTF16CHAR*>(Bytes.GetData() + 2);
				Text = FString(StringCast<TCHAR>(Wide, NumChars).Get(), NumChars);
			}
			else
			{
				Text = FString(StringCast<TCHAR>(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num()).Get(), Bytes.Num());
			}
			FTCHARToUTF8 Utf8(*Text);
			TArray<uint8> Converted;
			Converted.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());

			FSourceKeyValues Root;
			if (FSourceKeyValues::ParseSingle(Converted, Root, nullptr))
			{
				if (const FSourceKeyValues* TokenSection = Root.FindChild(TEXT("Tokens")))
				{
					for (const FSourceKeyValues& Entry : TokenSection->Children)
					{
						if (!Entry.IsSection())
						{
							Tokens.Add(Entry.Key.ToLower(), Entry.Value);
						}
					}
				}
			}
		}
		UE_LOG(LogLambda, Log, TEXT("Main menu: %d localised strings"), Tokens.Num());
	}

	if (const FString* Found = Tokens.Find(Token.ToLower()))
	{
		return *Found;
	}
	// No localisation to hand: show the token without its marker rather than nothing at all.
	return Token;
}

// The two commands Source drives the menu with (engine/vgui_baseui_interface.cpp). Escape runs gameui_activate
// there too - sys_mainwind.cpp does exactly that - so the key and the command go the same way in.
static ULambdaMainMenu* GetMenu(UWorld* World)
{
	UGameInstance* Instance = World ? World->GetGameInstance() : nullptr;
	return Instance ? Instance->GetSubsystem<ULambdaMainMenu>() : nullptr;
}

static FAutoConsoleCommandWithWorld GLambdaGameUIActivate(
	TEXT("gameui_activate"),
	TEXT("Shows the game UI"),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (ULambdaMainMenu* Menu = GetMenu(World))
		{
			Menu->ShowPauseMenu();
		}
	}));

static FAutoConsoleCommandWithWorld GLambdaGameUIHide(
	TEXT("gameui_hide"),
	TEXT("Hides the game UI"),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		if (ULambdaMainMenu* Menu = GetMenu(World))
		{
			Menu->Hide();
		}
	}));
