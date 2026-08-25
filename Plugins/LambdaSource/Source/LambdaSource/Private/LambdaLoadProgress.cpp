#include "LambdaLoadProgress.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

namespace
{
	/** How much of the bar each stage owns, and what it says while it is running. */
	struct FStageSlice
	{
		float Start;
		float End;
		const TCHAR* Status;
	};

	FStageSlice SliceFor(ELambdaLoadStage Stage)
	{
		// The weights are roughly what the stages cost on a map with props in it: reading the file is quick,
		// building the world is a fair chunk, and spawning entities dominates because that is where every model
		// is loaded and every material compiled.
		switch (Stage)
		{
		case ELambdaLoadStage::ReadingMap:			return { 0.00f, 0.15f, TEXT("Loading world model") };
		case ELambdaLoadStage::BuildingWorld:		return { 0.15f, 0.45f, TEXT("Building world") };
		case ELambdaLoadStage::SpawningEntities:	return { 0.45f, 0.92f, TEXT("Initializing level") };
		case ELambdaLoadStage::Precaching:			return { 0.92f, 1.00f, TEXT("Precaching resources") };
		case ELambdaLoadStage::Done:				return { 1.00f, 1.00f, TEXT("") };
		default:									return { 0.00f, 0.00f, TEXT("") };
		}
	}

	struct FState
	{
		FCriticalSection Lock;
		ELambdaLoadStage Stage = ELambdaLoadStage::Idle;
		float Fraction = 0.0f;
		bool bMeasured = false;
		FString Status;
		FString MapName;
	};

	FState& Get()
	{
		static FState State;
		return State;
	}
}

void FLambdaLoadProgress::Begin(const FString& MapName)
{
	FState& State = Get();
	FScopeLock Guard(&State.Lock);
	State.Stage = ELambdaLoadStage::Idle;
	State.Fraction = 0.0f;
	State.bMeasured = false;
	State.Status.Reset();
	State.MapName = MapName;
}

void FLambdaLoadProgress::SetStage(ELambdaLoadStage Stage)
{
	const FStageSlice Slice = SliceFor(Stage);
	FState& State = Get();
	FScopeLock Guard(&State.Lock);
	State.Stage = Stage;
	State.Fraction = Slice.Start;
	State.bMeasured = true;
	State.Status = Slice.Status;
}

void FLambdaLoadProgress::SetStageFraction(float Fraction)
{
	FState& State = Get();
	FScopeLock Guard(&State.Lock);
	const FStageSlice Slice = SliceFor(State.Stage);
	// The bar only ever goes forwards, so a stage that reports out of order cannot pull it backwards.
	const float Within = Slice.Start + (Slice.End - Slice.Start) * FMath::Clamp(Fraction, 0.0f, 1.0f);
	State.Fraction = FMath::Max(State.Fraction, Within);
}

void FLambdaLoadProgress::End()
{
	FState& State = Get();
	FScopeLock Guard(&State.Lock);
	State.Stage = ELambdaLoadStage::Done;
	State.Fraction = 1.0f;
	State.Status.Reset();
}

float FLambdaLoadProgress::GetFraction()
{
	FState& State = Get();
	FScopeLock Guard(&State.Lock);
	return State.Fraction;
}

bool FLambdaLoadProgress::IsMeasured()
{
	FState& State = Get();
	FScopeLock Guard(&State.Lock);
	return State.bMeasured;
}

FString FLambdaLoadProgress::GetStatus()
{
	FState& State = Get();
	FScopeLock Guard(&State.Lock);
	return State.Status;
}

FString FLambdaLoadProgress::GetMapName()
{
	FState& State = Get();
	FScopeLock Guard(&State.Lock);
	return State.MapName;
}
