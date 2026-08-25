#include "Core/SourceCoordinates.h"
#include "Core/LambdaSourceSettings.h"

float FSourceCoords::GetUnitScale()
{
	const ULambdaSourceSettings* Settings = GetDefault<ULambdaSourceSettings>();
	const float Scale = Settings ? Settings->UnitScale : 1.905f;
	return Scale > KINDA_SMALL_NUMBER ? Scale : 1.905f;
}

bool FSourceCoords::ParseFloats(const FString& Text, TArray<float>& Out)
{
	Out.Reset();
	TArray<FString> Parts;
	FString Cleaned = Text;
	Cleaned.ReplaceInline(TEXT(","), TEXT(" "));
	Cleaned.ParseIntoArrayWS(Parts);
	for (const FString& Part : Parts)
	{
		if (Part.IsEmpty())
		{
			continue;
		}
		Out.Add(FCString::Atof(*Part));
	}
	return Out.Num() > 0;
}

bool FSourceCoords::ParseVector(const FString& Text, FVector3f& Out)
{
	TArray<float> Values;
	if (!ParseFloats(Text, Values) || Values.Num() < 3)
	{
		return false;
	}
	Out = FVector3f(Values[0], Values[1], Values[2]);
	return true;
}
