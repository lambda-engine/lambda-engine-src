#include "SourceEntity.h"
#include "LambdaSourceModule.h"
#include "SourceBSPWorldActor.h"

void ASourceEntity::InitializeEntity(const FSourceEntity& InEntity, ASourceBSPWorldActor* InWorldActor)
{
	Entity = InEntity;
	WorldActor = InWorldActor;
	TargetName = Entity.Get(TEXT("targetname"));
	SpawnFlags = Entity.GetInt(TEXT("spawnflags"), 0);

	// Every Source output keyvalue is named "On<Something>" (OnPressed, OnFullyOpen, ...). Without a datadesc to
	// consult we use that convention to tell connections apart from ordinary keyvalues.
	Outputs.Reset();
	for (const TPair<FString, FString>& Pair : Entity.Pairs)
	{
		if (!Pair.Key.StartsWith(TEXT("On")) || Pair.Value.IsEmpty())
		{
			continue;
		}
		FSourceOutput* Output = Outputs.FindByPredicate([&Pair](const FSourceOutput& O) { return O.Name.Equals(Pair.Key, ESearchCase::IgnoreCase); });
		if (!Output)
		{
			Output = &Outputs.AddDefaulted_GetRef();
			Output->Name = Pair.Key;
		}
		const FSourceEventAction Action = FSourceEventAction::Parse(Pair.Value);
		Output->Actions.Add(Action);
		UE_LOG(LogLambdaSource, Verbose, TEXT("  output %s -> %s.%s(%s) delay=%g times=%d"),
			*Pair.Key, *Action.Target, *Action.TargetInput, *Action.Parameter, Action.Delay, Action.TimesToFire);
	}
}

bool ASourceEntity::MatchesTargetName(const FString& Pattern) const
{
	if (TargetName.IsEmpty() || Pattern.IsEmpty())
	{
		return false;
	}
	if (Pattern.EndsWith(TEXT("*")))
	{
		return TargetName.StartsWith(Pattern.LeftChop(1), ESearchCase::IgnoreCase);
	}
	return TargetName.Equals(Pattern, ESearchCase::IgnoreCase);
}

bool ASourceEntity::AcceptInput(const FString& InputName, AActor* Activator, AActor* Caller, const FString& Parameter)
{
	UE_LOG(LogLambdaSource, Verbose, TEXT("%s '%s': unhandled input '%s'"), *Entity.ClassName, *TargetName, *InputName);
	return false;
}

void ASourceEntity::FireOutput(const FString& OutputName, AActor* Activator, float ExtraDelay)
{
	ASourceBSPWorldActor* World = WorldActor.Get();
	if (!World)
	{
		return;
	}
	for (FSourceOutput& Output : Outputs)
	{
		if (!Output.Name.Equals(OutputName, ESearchCase::IgnoreCase))
		{
			continue;
		}
		for (FSourceEventAction& Action : Output.Actions)
		{
			// Respect a limited fire count (CEventAction::m_nTimesToFire).
			if (Action.FiresLeft != SOURCE_EVENT_FIRE_ALWAYS)
			{
				if (Action.FiresLeft <= 0)
				{
					continue;
				}
				--Action.FiresLeft;
			}
			World->QueueEntityEvent(Action.Target, Action.TargetInput, Action.Parameter, Activator, this, Action.Delay + ExtraDelay);
		}
	}
}
