#include "LambdaMainMenu.h"

#include "LambdaConsole.h"
#include "LambdaEngine.h"
#include "LambdaFileSystem.h"
#include "LambdaSourceSettings.h"
#include "SourceKeyValues.h"

#include "Engine/GameInstance.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "GameMapsSettings.h"
#include "Kismet/GameplayStatics.h"

// SourceScheme.res again: the menu is written in the same off-white, and whatever the mouse is over goes gold.
FLinearColor ULambdaMainMenu::ItemColour()     { return FLinearColor(216 / 255.0f, 222 / 255.0f, 211 / 255.0f, 1.0f); }	// BaseText
FLinearColor ULambdaMainMenu::SelectedColour() { return FLinearColor(196 / 255.0f, 181 / 255.0f,  80 / 255.0f, 1.0f); }	// BrightControlText
FLinearColor ULambdaMainMenu::TitleColour()    { return FLinearColor(1.0f, 176 / 255.0f, 0.0f, 1.0f); }

/** Mouse and keys for the menu. Sits behind the console, which takes the keyboard first when it is open. */
class FLambdaMenuInput : public IInputProcessor
{
public:
	explicit FLambdaMenuInput(ULambdaMainMenu* InMenu) : Menu(InMenu) {}

	virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override {}

	virtual bool HandleKeyDownEvent(FSlateApplication&, const FKeyEvent& KeyEvent) override
	{
		ULambdaMainMenu* M = Menu.Get();
		if (!M || !M->IsActive() || IsConsoleOpen(M))
		{
			return false;
		}
		const FKey Key = KeyEvent.GetKey();
		if (Key == EKeys::Up)    { M->MoveSelection(-1); return true; }
		if (Key == EKeys::Down)  { M->MoveSelection(+1); return true; }
		if (Key == EKeys::Enter) { M->Activate();        return true; }
		return false;
	}

	virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
	{
		ULambdaMainMenu* M = Menu.Get();
		if (M && M->IsActive() && !IsConsoleOpen(M))
		{
			M->SelectAt(ToViewport(SlateApp, MouseEvent));
		}
		return false;
	}

	virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
	{
		ULambdaMainMenu* M = Menu.Get();
		if (!M || !M->IsActive() || IsConsoleOpen(M) || MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
		{
			return false;
		}
		if (M->SelectAt(ToViewport(SlateApp, MouseEvent)))
		{
			M->Activate();
			return true;
		}
		return false;
	}

private:
	static bool IsConsoleOpen(ULambdaMainMenu* M)
	{
		const ULambdaConsole* Console = M->GetGameInstance() ? M->GetGameInstance()->GetSubsystem<ULambdaConsole>() : nullptr;
		return Console && Console->IsOpen();
	}

	/** Slate gives desktop pixels; the menu was drawn in the game window's. */
	static FVector2D ToViewport(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
	{
		const FVector2D Screen = MouseEvent.GetScreenSpacePosition();
		TSharedPtr<SWindow> Window = SlateApp.GetActiveTopLevelWindow();
		if (Window.IsValid())
		{
			const FGeometry& Geometry = Window->GetWindowGeometryInScreen();
			return (Screen - Geometry.GetAbsolutePosition()) / Geometry.Scale;
		}
		return Screen;
	}

	TWeakObjectPtr<ULambdaMainMenu> Menu;
};

void ULambdaMainMenu::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (FSlateApplication::IsInitialized())
	{
		InputProcessor = MakeShared<FLambdaMenuInput>(this);
		// Behind the console, which is registered at 0 and swallows the keyboard while it is open.
		FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor, 1);
	}
}

void ULambdaMainMenu::Deinitialize()
{
	if (InputProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
	}
	InputProcessor.Reset();
	Super::Deinitialize();
}

void ULambdaMainMenu::Show()
{
	LoadItems();
	Selected = 0;
	bActive = true;

	if (UGameInstance* Instance = GetGameInstance())
	{
		if (APlayerController* PC = Instance->GetFirstLocalPlayerController())
		{
			PC->bShowMouseCursor = true;
			PC->SetIgnoreLookInput(true);
			PC->SetIgnoreMoveInput(true);
		}
	}
	UE_LOG(LogLambda, Log, TEXT("Main menu: %d items"), Items.Num());
}

void ULambdaMainMenu::Hide()
{
	bActive = false;
	if (UGameInstance* Instance = GetGameInstance())
	{
		if (APlayerController* PC = Instance->GetFirstLocalPlayerController())
		{
			PC->bShowMouseCursor = false;
			PC->SetIgnoreLookInput(false);
			PC->SetIgnoreMoveInput(false);
		}
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

void ULambdaMainMenu::LoadItems()
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

			// Not in a game, so the ones that only make sense in one are left out - and the console-only and
			// VR entries with them, which this engine has nothing to say about.
			const bool bConsoleOnly = Entry.GetString(TEXT("ConsoleOnly")) == TEXT("1");
			const bool bVR = !Entry.GetString(TEXT("OnlyWhenVREnabled")).IsEmpty();
			if (Item.bOnlyInGame || bConsoleOnly || bVR || Item.Command.IsEmpty() || Item.Label.IsEmpty())
			{
				continue;
			}
			Items.Add(MoveTemp(Item));
		}
	}

	if (Items.Num() == 0)
	{
		// Nothing to read, so the menu is the one Half-Life 2 has out of a game.
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
