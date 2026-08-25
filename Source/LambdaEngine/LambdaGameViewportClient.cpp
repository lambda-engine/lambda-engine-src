#include "LambdaGameViewportClient.h"

#include "LambdaConsole.h"
#include "LambdaMainMenu.h"

#include "Engine/GameInstance.h"

ULambdaConsole* ULambdaGameViewportClient::GetConsole() const
{
	return GameInstance ? GameInstance->GetSubsystem<ULambdaConsole>() : nullptr;
}

ULambdaMainMenu* ULambdaGameViewportClient::GetMenu() const
{
	return GameInstance ? GameInstance->GetSubsystem<ULambdaMainMenu>() : nullptr;
}

bool ULambdaGameViewportClient::InputChar(FViewport* InViewport, int32 ControllerId, TCHAR Character)
{
	ULambdaConsole* Console = GetConsole();
	if (Console && Console->IsOpen())
	{
		// The key that opens the console also produces a character; it should not end up in the entry box.
		if (Character >= 32 && Character != TEXT('`') && Character != TEXT('~'))
		{
			Console->TypeCharacter(Character);
		}
		return true;
	}
	return Super::InputChar(InViewport, ControllerId, Character);
}

bool ULambdaGameViewportClient::InputKey(const FInputKeyEventArgs& EventArgs)
{
	ULambdaConsole* Console = GetConsole();
	ULambdaMainMenu* Menu = GetMenu();
	const FKey Key = EventArgs.Key;
	const bool bPressed = EventArgs.Event == IE_Pressed || EventArgs.Event == IE_Repeat;

	// ---- the console, which takes the keyboard off everything else while it is open ----
	if (Console && bPressed && Key == EKeys::Tilde)
	{
		Console->Toggle();
		return true;
	}
	if (Console && Console->IsOpen())
	{
		if (Key == EKeys::MouseScrollUp)   { Console->Scroll(+3); return true; }
		if (Key == EKeys::MouseScrollDown) { Console->Scroll(-3); return true; }
		if (bPressed)
		{
			if (Key == EKeys::Escape)    { Console->SetOpen(false); return true; }
			if (Key == EKeys::Enter)     { Console->Submit();       return true; }
			if (Key == EKeys::BackSpace) { Console->Backspace();    return true; }
			if (Key == EKeys::Up)        { Console->HistoryBack();  return true; }
			if (Key == EKeys::Down)      { Console->HistoryForward(); return true; }
			if (Key == EKeys::PageUp)    { Console->Scroll(+5);     return true; }
			if (Key == EKeys::PageDown)  { Console->Scroll(-5);     return true; }
		}
		// Everything else is swallowed so the game does not act on what is being typed. The characters
		// themselves arrive through InputChar.
		return true;
	}

	// ---- the menu ----
	if (Menu && Menu->IsActive())
	{
		if (bPressed && Key == EKeys::Up)    { Menu->MoveSelection(-1); return true; }
		if (bPressed && Key == EKeys::Down)  { Menu->MoveSelection(+1); return true; }
		if (bPressed && Key == EKeys::Enter) { Menu->Activate();        return true; }
		if (bPressed && Key == EKeys::Escape)
		{
			// Escape backs out of the pause menu, the way it opened it. On the main menu there is nothing to
			// go back to.
			if (Menu->IsPauseMenu())
			{
				Menu->Hide();
			}
			return true;
		}
		if (bPressed && Key == EKeys::LeftMouseButton)
		{
			FVector2D MousePosition;
			if (GetMousePosition(MousePosition) && Menu->SelectAt(MousePosition))
			{
				Menu->Activate();
			}
			return true;
		}
		return Super::InputKey(EventArgs);
	}

	// ---- in a game, with nothing over it: Escape brings the menu up and stops the world ----
	if (Menu && bPressed && Key == EKeys::Escape)
	{
		Menu->ShowPauseMenu();
		return true;
	}

	return Super::InputKey(EventArgs);
}
