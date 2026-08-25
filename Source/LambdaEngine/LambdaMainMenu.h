#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "LambdaMainMenu.generated.h"

/** One line of the menu, as resource/GameMenu.res describes it. */
struct FLambdaMenuItem
{
	FString Label;			// what it says, with the #token already looked up
	FString Command;		// what it does
	bool bOnlyInGame = false;	// hidden until there is a game to go back to
	bool bNotMulti = false;
	int32 InGameOrder = 0;

	/** Where it was last drawn, so a click can be matched to it. */
	FBox2D Bounds = FBox2D(ForceInit);
};

/**
 * The main menu.
 *
 * Source builds this from resource/GameMenu.res - a list of labels and the commands behind them - and looks the
 * labels up in resource/gameui_english.txt, which is where "NEW GAME" and the rest come from. Both are read here
 * for the same reason: the menu is then whatever the game directory says it is, and a mod can change it without
 * changing the engine. There is a built-in list for a game directory that has neither.
 */
UCLASS()
class LAMBDAENGINE_API ULambdaMainMenu : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	bool IsActive() const { return bActive; }
	/** True when it came up over a running game, which is the only time there is anything to go back to. */
	bool IsPauseMenu() const { return bPauseMenu; }
	/** The menu the game starts on. */
	void Show();
	/** The same menu over a running game, which stops while it is up - Source pauses single player too. */
	void ShowPauseMenu();
	void Hide();
	/** Keeps the cursor and the input state right; the controller does not exist yet when the menu first opens. */
	void TickInputState();

	/** The items that should be on screen right now (OnlyInGame ones are not, out of a game). */
	const TArray<FLambdaMenuItem>& GetItems() const { return Items; }
	TArray<FLambdaMenuItem>& GetMutableItems() { return Items; }

	int32 GetSelected() const { return Selected; }
	void MoveSelection(int32 Delta);
	void SetSelected(int32 Index);
	/** Points at whatever is under the cursor, returning true if that changed anything. */
	bool SelectAt(const FVector2D& Position);
	/** Runs the selected item's command. */
	void Activate();
	void RunCommand(const FString& Command);

	/** Source's menu colours, from platform/resource/SourceScheme.res. */
	static FLinearColor ItemColour();		// BaseText
	static FLinearColor SelectedColour();	// BrightControlText
	static FLinearColor TitleColour();

private:
	void LoadItems(bool bInGame);
	void SetPaused(bool bPaused);
	/** Looks a "#GameUI_..." label up in the game directory's localisation file. */
	static FString ResolveLabel(const FString& Label);

	TArray<FLambdaMenuItem> Items;
	int32 Selected = 0;
	bool bActive = false;
	bool bPauseMenu = false;

};
