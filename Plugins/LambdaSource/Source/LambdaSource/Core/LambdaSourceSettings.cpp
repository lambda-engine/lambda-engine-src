#include "Core/LambdaSourceSettings.h"

ULambdaSourceSettings::ULambdaSourceSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("Lambda Source");

	GameDirectories.Add(TEXT("../lambda-engine/Mods/lambda"));
	MasterMaterial = FSoftObjectPath(TEXT("/LambdaSource/Materials/M_LambdaBase.M_LambdaBase"));
	DecalMaterial = FSoftObjectPath(TEXT("/LambdaSource/Materials/M_LambdaDecal.M_LambdaDecal"));
	DecalPBRMaterial = FSoftObjectPath(TEXT("/LambdaSource/Materials/M_LambdaDecalPBR.M_LambdaDecalPBR"));
	SpriteMaterial = FSoftObjectPath(TEXT("/LambdaSource/Materials/M_LambdaSprite.M_LambdaSprite"));
	SpriteMaterialNoZ = FSoftObjectPath(TEXT("/LambdaSource/Materials/M_LambdaSpriteNoZ.M_LambdaSpriteNoZ"));
	SpriteMaterialTranslucent = FSoftObjectPath(TEXT("/LambdaSource/Materials/M_LambdaSpriteTranslucent.M_LambdaSpriteTranslucent"));
	ModelMaterial = FSoftObjectPath(TEXT("/LambdaSource/Materials/M_LambdaModel.M_LambdaModel"));
	ModelMaterialTranslucent = FSoftObjectPath(TEXT("/LambdaSource/Materials/M_LambdaModelTranslucent.M_LambdaModelTranslucent"));
	ModelMaterialMasked = FSoftObjectPath(TEXT("/LambdaSource/Materials/M_LambdaModelMasked.M_LambdaModelMasked"));
	FallbackMaterial = FSoftObjectPath(TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
}
