#include "LambdaSourceSettings.h"

ULambdaSourceSettings::ULambdaSourceSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("Lambda Source");

	GameDirectories.Add(TEXT("../game/Game/lambda"));
	MasterMaterial = FSoftObjectPath(TEXT("/LambdaSource/Materials/M_LambdaBase.M_LambdaBase"));
	DecalMaterial = FSoftObjectPath(TEXT("/LambdaSource/Materials/M_LambdaDecal.M_LambdaDecal"));
	DecalPBRMaterial = FSoftObjectPath(TEXT("/LambdaSource/Materials/M_LambdaDecalPBR.M_LambdaDecalPBR"));
	SpriteMaterial = FSoftObjectPath(TEXT("/LambdaSource/Materials/M_LambdaSprite.M_LambdaSprite"));
	SpriteMaterialNoZ = FSoftObjectPath(TEXT("/LambdaSource/Materials/M_LambdaSpriteNoZ.M_LambdaSpriteNoZ"));
	FallbackMaterial = FSoftObjectPath(TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
}
