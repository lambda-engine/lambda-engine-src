#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "LambdaConsole.generated.h"

/** One line the console has said, kept with the colour it was said in (CConsolePanel::ColorPrint). */
struct FLambdaConsoleLine
{
	FString Text;
	FLinearColor Color = FLinearColor::White;
};

/**
 * The developer console.
 *
 * Source's console is a panel over the game that takes a command line, echoes it back with "] " in front, and
 * keeps what it has said so you can scroll back through it (vgui_controls/consoledialog.cpp). This is that: the
 * text, the history, and the one command that does anything so far.
 *
 * It lives on the game instance rather than the world because "map" travels to a new level, and a console that
 * forgot everything the moment it was used would be no use at all.
 */
UCLASS()
class LAMBDAENGINE_API ULambdaConsole : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	bool IsOpen() const { return bOpen; }
	void SetOpen(bool bInOpen);
	void Toggle() { SetOpen(!bOpen); }

	/** Says something, in the colour ordinary output is said in. */
	void Print(const FString& Text);
	void ColorPrint(const FLinearColor& Color, const FString& Text);

	/** Runs one command line, the way submitting the entry box does. */
	void Execute(const FString& CommandLine);

	// ---- what the entry box is doing ----
	void TypeCharacter(TCHAR Character);
	void Backspace();
	void ClearInput();
	void Submit();
	/** Up and down walk back and forth through what has been typed before. */
	void HistoryBack();
	void HistoryForward();
	/** The wheel and the page keys move the view without disturbing the entry. */
	void Scroll(int32 Lines);

	const TArray<FLambdaConsoleLine>& GetLines() const { return Lines; }
	const FString& GetInput() const { return Input; }
	int32 GetScrollBack() const { return ScrollBack; }

	/** Source's console colours, from platform/resource/SourceScheme.res. */
	static FLinearColor TextColour();		// BaseText
	static FLinearColor EchoColour();		// BrightControlText - the command as it is echoed back
	static FLinearColor WarningColour();
	static FLinearColor BackgroundColour();	// WindowBG

private:
	TArray<FLambdaConsoleLine> Lines;
	FString Input;
	TArray<FString> History;
	/** Where up/down have walked to; one past the end means "at the empty line you are typing". */
	int32 HistoryPosition = 0;
	int32 ScrollBack = 0;
	bool bOpen = false;

	TSharedPtr<class FLambdaConsoleInput> InputProcessor;

	/** How many lines are kept before the oldest are forgotten. */
	static constexpr int32 MaxLines = 1024;
};
