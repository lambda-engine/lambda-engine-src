#include "LambdaFonts.h"

#include "LambdaEngine.h"
#include "LambdaFileSystem.h"

#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "UObject/Package.h"

UFont* FLambdaFonts::GetSchemeFont()
{
	static TObjectPtr<UFont> Font = nullptr;
	static bool bTried = false;
	if (bTried)
	{
		return Font;
	}
	bTried = true;

	TArray<uint8> FontData;
	if (!FLambdaFileSystem::Get().ReadFile(TEXT("resource/din1451alt.ttf"), FontData) || FontData.Num() == 0)
	{
		UE_LOG(LogLambda, Log, TEXT("UI: no resource/din1451alt.ttf in the game directory; falling back to the engine font"));
		return nullptr;
	}

	UFontFace* Face = NewObject<UFontFace>(GetTransientPackage());
	Face->LoadingPolicy = EFontLoadingPolicy::Inline;
	Face->FontFaceData = FFontFaceData::MakeFontFaceData(MoveTemp(FontData));

	UFont* NewFont = NewObject<UFont>(GetTransientPackage());
	NewFont->FontCacheType = EFontCacheType::Runtime;
	FTypefaceEntry& Entry = NewFont->GetMutableInternalCompositeFont().DefaultTypeface.Fonts.AddDefaulted_GetRef();
	Entry.Name = TEXT("Regular");
	Entry.Font = FFontData(Face);
	NewFont->LegacyFontSize = 31;	// HudTextLarge's "tall", which is what the HUD's numbers are drawn at

	// Rooted: it outlives every world, and nothing else holds a reference to it.
	Face->AddToRoot();
	NewFont->AddToRoot();
	Font = NewFont;

	UE_LOG(LogLambda, Log, TEXT("UI: using the scheme font resource/din1451alt.ttf"));
	return Font;
}
