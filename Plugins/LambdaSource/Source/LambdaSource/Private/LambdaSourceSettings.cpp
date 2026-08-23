#include "LambdaSourceSettings.h"

ULambdaSourceSettings::ULambdaSourceSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("Lambda Source");

	GameDirectories.Add(TEXT("../game/Game/lambda"));
	MasterMaterial = FSoftObjectPath(TEXT("/LambdaSource/Materials/M_LambdaBase.M_LambdaBase"));
	FallbackMaterial = FSoftObjectPath(TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
}
