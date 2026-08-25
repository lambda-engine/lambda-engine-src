#include "LambdaUITextures.h"

#include "LambdaEngine.h"
#include "FileSystem/LambdaFileSystem.h"
#include "Materials/LambdaMaterialLibrary.h"
#include "Formats/SourceKeyValues.h"
#include "Formats/SourceVTFFile.h"

#include "Engine/Texture2D.h"

UTexture2D* FLambdaUITextures::Get(const FString& MaterialName)
{
	static TMap<FString, TObjectPtr<UTexture2D>> Cache;
	const FString Key = MaterialName.ToLower().Replace(TEXT("\\"), TEXT("/"));
	if (TObjectPtr<UTexture2D>* Found = Cache.Find(Key))
	{
		return *Found;
	}
	// Whatever happens below, it happens once - a missing file is remembered as missing rather than looked for
	// again on every map load.
	Cache.Add(Key, nullptr);

	const FLambdaFileSystem& Files = FLambdaFileSystem::Get();

	// The VMT names the texture; without one, the material name is taken as the texture name.
	FString TextureName = Key;
	TArray<uint8> VMTBytes;
	FSourceKeyValues VMT;
	if (Files.ReadFile(FString::Printf(TEXT("materials/%s.vmt"), *Key), VMTBytes)
		&& FSourceKeyValues::ParseSingle(VMTBytes, VMT, nullptr))
	{
		const FString Base = VMT.GetString(TEXT("$basetexture"));
		if (!Base.IsEmpty())
		{
			TextureName = Base.ToLower().Replace(TEXT("\\"), TEXT("/"));
		}
	}

	TArray<uint8> VTFBytes;
	const FString Path = FString::Printf(TEXT("materials/%s.vtf"), *TextureName);
	if (!Files.ReadFile(Path, VTFBytes))
	{
		UE_LOG(LogLambda, Log, TEXT("UI texture '%s': no '%s' in the game directory"), *MaterialName, *Path);
		return nullptr;
	}

	FSourceVTFFile VTF;
	FString Error;
	if (!VTF.Load(MoveTemp(VTFBytes), &Error))
	{
		UE_LOG(LogLambda, Warning, TEXT("UI texture '%s': %s would not load: %s"), *MaterialName, *Path, *Error);
		return nullptr;
	}

	UTexture2D* Texture = ULambdaMaterialLibrary::CreateTextureFromVTF(VTF, MaterialName, &Error);
	if (!Texture)
	{
		UE_LOG(LogLambda, Warning, TEXT("UI texture '%s': %s would not become a texture: %s"), *MaterialName, *Path, *Error);
		return nullptr;
	}

	Texture->AddToRoot();
	Cache.Add(Key, Texture);
	UE_LOG(LogLambda, Log, TEXT("UI texture '%s': %dx%d from %s"), *MaterialName,
		Texture->GetSizeX(), Texture->GetSizeY(), *Path);
	return Texture;
}
