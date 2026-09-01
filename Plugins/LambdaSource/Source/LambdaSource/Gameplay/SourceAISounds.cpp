#include "Gameplay/SourceAISounds.h"

#include "Core/LambdaSourceSettings.h"

FSourceAISounds& FSourceAISounds::Get()
{
	static FSourceAISounds Instance;
	return Instance;
}

void FSourceAISounds::Insert(ESourceAISoundType Type, const FVector& Position, float RadiusUnits,
	float DurationSeconds, const AActor* Owner, const UWorld* World)
{
	if (!World)
	{
		return;
	}
	const float Now = World->GetTimeSeconds();
	FEntry Entry;
	Entry.Position = Position;
	Entry.RadiusCm = RadiusUnits * ULambdaSourceSettings::Get().UnitScale;
	Entry.ExpireTime = Now + DurationSeconds;
	Entry.Type = Type;
	Entry.Owner = Owner;

	// Reuse the first slot whose sound has already stopped mattering; failing that, the oldest.
	int32 Slot = INDEX_NONE;
	float Oldest = TNumericLimits<float>::Max();
	for (int32 i = 0; i < Sounds.Num(); ++i)
	{
		if (Sounds[i].ExpireTime <= Now)
		{
			Slot = i;
			break;
		}
		if (Sounds[i].ExpireTime < Oldest)
		{
			Oldest = Sounds[i].ExpireTime;
			Slot = i;
		}
	}
	if (Sounds.Num() < MaxSounds)
	{
		Sounds.Add(MoveTemp(Entry));
	}
	else if (Slot != INDEX_NONE)
	{
		Sounds[Slot] = MoveTemp(Entry);
	}
}

bool FSourceAISounds::Loudest(const FVector& ListenerPos, const AActor* Listener, const UWorld* World,
	FVector& OutPosition, ESourceAISoundType& OutType) const
{
	if (!World)
	{
		return false;
	}
	const float Now = World->GetTimeSeconds();
	bool bFound = false;
	int32 BestRank = -1;
	float BestDistance = TNumericLimits<float>::Max();

	for (const FEntry& Entry : Sounds)
	{
		if (Entry.ExpireTime <= Now || Entry.Owner.Get() == Listener)
		{
			continue;	// over with, or one's own footsteps
		}
		const float Distance = FVector::Dist(Entry.Position, ListenerPos);
		if (Distance > Entry.RadiusCm)
		{
			continue;	// too far away to have carried this far
		}
		// Violence first, then whatever is nearest.
		const int32 Rank = (Entry.Type == ESourceAISoundType::Combat) ? 2
			: (Entry.Type == ESourceAISoundType::Danger) ? 1 : 0;
		if (Rank > BestRank || (Rank == BestRank && Distance < BestDistance))
		{
			BestRank = Rank;
			BestDistance = Distance;
			OutPosition = Entry.Position;
			OutType = Entry.Type;
			bFound = true;
		}
	}
	return bFound;
}
