#include "LambdaFonts.h"

#include "LambdaEngine.h"
#include "LambdaFileSystem.h"

#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "UObject/Package.h"

UFont* FLambdaFonts::Get(const FString& FileName)
{
	static TMap<FString, TObjectPtr<UFont>> Cache;
	const FString Key = FileName.ToLower();
	if (TObjectPtr<UFont>* Found = Cache.Find(Key))
	{
		return *Found;
	}
	// Whatever happens below happens once: a face that is not there is remembered as not there rather than
	// looked for again on every map load.
	Cache.Add(Key, nullptr);

	TArray<uint8> FontData;
	const FString Path = FString::Printf(TEXT("resource/%s"), *FileName);
	if (!FLambdaFileSystem::Get().ReadFile(Path, FontData) || FontData.Num() == 0)
	{
		UE_LOG(LogLambda, Log, TEXT("UI: no %s in the game directory"), *Path);
		return nullptr;
	}

	UFontFace* Face = NewObject<UFontFace>(GetTransientPackage());
	Face->LoadingPolicy = EFontLoadingPolicy::Inline;
	Face->FontFaceData = FFontFaceData::MakeFontFaceData(MoveTemp(FontData));

	UFont* Font = NewObject<UFont>(GetTransientPackage());
	Font->FontCacheType = EFontCacheType::Runtime;
	FTypefaceEntry& Entry = Font->GetMutableInternalCompositeFont().DefaultTypeface.Fonts.AddDefaulted_GetRef();
	Entry.Name = TEXT("Regular");
	Entry.Font = FFontData(Face);
	Font->LegacyFontSize = 31;	// HudTextLarge's "tall"; anything drawn through DrawTextAtHeight asks for its own

	// Rooted: they outlive every world, and nothing else holds a reference to them.
	Face->AddToRoot();
	Font->AddToRoot();
	Cache.Add(Key, Font);

	UE_LOG(LogLambda, Log, TEXT("UI: loaded %s"), *Path);
	return Font;
}

UFont* FLambdaFonts::GetSchemeFont()
{
	return Get(TEXT("din1451alt.ttf"));
}

UFont* FLambdaFonts::GetTitleFont()
{
	// IBM Plex Mono for the game's name. The scheme's face stands in if the mod has not got it.
	if (UFont* Font = Get(TEXT("IBMPlexMono-Bold.ttf")))
	{
		return Font;
	}
	return GetSchemeFont();
}

UFont* FLambdaFonts::GetMenuFont()
{
	if (UFont* Font = Get(TEXT("IBMPlexSans-Regular.ttf")))
	{
		return Font;
	}
	return GetSchemeFont();
}
