#include "World/SourceEntityIO.h"

FSourceEventAction FSourceEventAction::Parse(const FString& ActionData)
{
	// CEventAction::CEventAction - fields are target, input, parameter, delay, timesToFire.
	FSourceEventAction Action;

	// VMF_IOPARAM_STRING_DELIMITER (0x1B) is used when present so parameters can contain commas.
	const FString EscDelim = FString::Chr((TCHAR)0x1B);
	const FString Delim = ActionData.Contains(EscDelim) ? EscDelim : FString(TEXT(","));

	TArray<FString> Tokens;
	ActionData.ParseIntoArray(Tokens, *Delim, /*InCullEmpty=*/ false);

	auto Token = [&Tokens](int32 Index) -> FString
	{
		return Tokens.IsValidIndex(Index) ? Tokens[Index].TrimStartAndEnd() : FString();
	};

	Action.Target = Token(0);

	const FString Input = Token(1);
	Action.TargetInput = Input.IsEmpty() ? TEXT("Use") : Input;

	Action.Parameter = Token(2);

	const FString DelayText = Token(3);
	if (!DelayText.IsEmpty())
	{
		Action.Delay = FCString::Atof(*DelayText);
	}

	const FString TimesText = Token(4);
	if (!TimesText.IsEmpty())
	{
		Action.TimesToFire = FCString::Atoi(*TimesText);
		if (Action.TimesToFire == 0)
		{
			Action.TimesToFire = SOURCE_EVENT_FIRE_ALWAYS;
		}
	}
	Action.FiresLeft = Action.TimesToFire;
	return Action;
}
