#pragma once

#include "CoreMinimal.h"
#include "Engine/GameViewportClient.h"
#include "LambdaGameViewportClient.generated.h"

/**
 * Where the console and the menu get their input.
 *
 * The viewport is the first thing the engine offers a key press to, and - unlike a Slate input pre-processor -
 * it is also offered the character the keyboard actually produced, with shift and the layout already applied.
 * That is the difference between being able to type "npc_headcrab" and not: an underscore is Shift and a key
 * whose name says nothing about underscores, and on a keyboard that is not American it may not be that key at
 * all.
 */
UCLASS()
class LAMBDAENGINE_API ULambdaGameViewportClient : public UGameViewportClient
{
	GENERATED_BODY()

public:
	virtual bool InputKey(const FInputKeyEventArgs& EventArgs) override;
	virtual bool InputChar(FViewport* InViewport, int32 ControllerId, TCHAR Character) override;

private:
	class ULambdaConsole* GetConsole() const;
	class ULambdaMainMenu* GetMenu() const;
};
