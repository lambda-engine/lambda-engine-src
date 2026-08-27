#include "LambdaMenuScheme.h"

#include "LambdaEngine.h"
#include "LambdaFonts.h"
#include "LambdaUITextures.h"
#include "FileSystem/LambdaFileSystem.h"
#include "Formats/SourceKeyValues.h"

#include "Misc/FileHelper.h"

namespace
{
	/**
	 * A file out of the mod's own folder, ignoring everything it mounts.
	 *
	 * The menu is the mod's own, and these are the files that describe it. Read through the search paths
	 * instead and a mod that mounts Half-Life 2 for its models and its textures would be handed Half-Life 2's
	 * menu with them - its title in the place and the face Half-Life 2 wants, which is never what mounting a
	 * game for its content was meant to ask for. The faces and the pictures these files name are still looked
	 * up everywhere, so a mod can still write its title in a face that came with the content it mounts.
	 */
	bool ReadModFile(const TCHAR* RelativePath, FSourceKeyValues& OutRoot)
	{
		const FString GameDirectory = FLambdaFileSystem::Get().GetGameDirectory();
		if (GameDirectory.IsEmpty())
		{
			return false;
		}

		TArray<uint8> Bytes;
		return FFileHelper::LoadFileToArray(Bytes, *(GameDirectory / RelativePath))
			&& FSourceKeyValues::ParseSingle(Bytes, OutRoot, nullptr);
	}

	/** The mod's own gameinfo.txt, parsed, or false if there is not one to parse. */
	bool ReadGameInfo(FSourceKeyValues& OutRoot)
	{
		return ReadModFile(TEXT("gameinfo.txt"), OutRoot);
	}

	/**
	 * A scheme colour: "255 176 0 255", or the name of one of the entries in the scheme's own Colors block,
	 * which is how Source lets a scheme name a colour once and use it everywhere.
	 */
	bool ParseSchemeColour(const FSourceKeyValues& Scheme, const FString& Value, FLinearColor& OutColour)
	{
		if (Value.IsEmpty())
		{
			return false;
		}

		TArray<FString> Parts;
		Value.ParseIntoArray(Parts, TEXT(" "), true);

		if (Parts.Num() < 3)
		{
			// Not numbers, so it is the name of a colour the scheme declares.
			if (const FSourceKeyValues* Colours = Scheme.FindChild(TEXT("Colors")))
			{
				const FString Named = Colours->GetString(*Value);
				if (!Named.IsEmpty() && Named != Value)
				{
					return ParseSchemeColour(Scheme, Named, OutColour);
				}
			}
			return false;
		}

		const float R = FCString::Atof(*Parts[0]) / 255.0f;
		const float G = FCString::Atof(*Parts[1]) / 255.0f;
		const float B = FCString::Atof(*Parts[2]) / 255.0f;
		const float A = Parts.Num() >= 4 ? FCString::Atof(*Parts[3]) / 255.0f : 1.0f;

		OutColour = FLinearColor(R, G, B, A);
		return true;
	}

	/**
	 * Reads "BaseSettings/<Key>" into Out if the scheme has it, leaving Out alone if it has not - which is what
	 * makes every default in the header the value a silent mod gets. Templated because an FVector2D is made of
	 * doubles and a font height is a float.
	 */
	template<typename NumberType>
	void ReadSetting(const FSourceKeyValues* BaseSettings, const TCHAR* Key, NumberType& Out)
	{
		if (!BaseSettings)
		{
			return;
		}
		const FString Value = BaseSettings->GetString(Key);
		if (!Value.IsEmpty())
		{
			Out = static_cast<NumberType>(FCString::Atod(*Value));
		}
	}

	void ReadSettingColour(const FSourceKeyValues& Scheme, const FSourceKeyValues* BaseSettings, const TCHAR* Key, FLinearColor& Out)
	{
		if (!BaseSettings)
		{
			return;
		}
		ParseSchemeColour(Scheme, BaseSettings->GetString(Key), Out);
	}

	/** Letters and digits only, lowercased, so "HalfLife2" and "HALFLIFE2.ttf" come out the same. */
	FString NormaliseFontName(const FString& Name)
	{
		FString Out;
		Out.Reserve(Name.Len());
		for (const TCHAR Character : Name)
		{
			if (FChar::IsAlnum(Character))
			{
				Out.AppendChar(FChar::ToLower(Character));
			}
		}
		return Out;
	}
}

const FLambdaMenuScheme& FLambdaMenuScheme::Get()
{
	static FLambdaMenuScheme Scheme;
	static bool bLoaded = false;
	if (bLoaded)
	{
		return Scheme;
	}
	bLoaded = true;

	// gameinfo.txt: the two lines of text. A mod that does not set "title" is titled by its own name, which is
	// what the menu showed before there was a title to set.
	FSourceKeyValues GameInfo;
	if (ReadGameInfo(GameInfo))
	{
		Scheme.Title1 = GameInfo.GetString(TEXT("title"));
		Scheme.Title2 = GameInfo.GetString(TEXT("title2"));
	}
	if (Scheme.Title1.IsEmpty())
	{
		Scheme.Title1 = FLambdaFileSystem::Get().GetGameName();
	}
	if (Scheme.Title1.IsEmpty())
	{
		Scheme.Title1 = TEXT("Lambda Engine");
	}

	// resource/clientscheme.res: where it goes, what colour it is and what it is written in.
	FSourceKeyValues Root;
	if (ReadModFile(TEXT("resource/clientscheme.res"), Root))
	{
		// Source wraps the whole thing in a "Scheme" section; a file that does not is read as it is.
		const FSourceKeyValues* SchemeKv = Root.FindChild(TEXT("Scheme"));
		const FSourceKeyValues& Body = SchemeKv ? *SchemeKv : Root;

		const FSourceKeyValues* BaseSettings = Body.FindChild(TEXT("BaseSettings"));
		ReadSetting(BaseSettings, TEXT("Main.Title1.X"), Scheme.Title1Position.X);
		ReadSetting(BaseSettings, TEXT("Main.Title1.Y"), Scheme.Title1Position.Y);
		ReadSettingColour(Body, BaseSettings, TEXT("Main.Title1.Color"), Scheme.Title1Colour);

		ReadSetting(BaseSettings, TEXT("Main.Title2.X"), Scheme.Title2Position.X);
		ReadSetting(BaseSettings, TEXT("Main.Title2.Y"), Scheme.Title2Position.Y);
		ReadSettingColour(Body, BaseSettings, TEXT("Main.Title2.Color"), Scheme.Title2Colour);

		// Fonts/ClientTitleFont/1 - the first entry is the one that applies at any screen size this engine runs
		// at; Source's later entries are there to switch faces below a resolution nothing uses any more.
		if (const FSourceKeyValues* Fonts = Body.FindChild(TEXT("Fonts")))
		{
			if (const FSourceKeyValues* TitleFont = Fonts->FindChild(TEXT("ClientTitleFont")))
			{
				if (const FSourceKeyValues* First = TitleFont->FindChild(TEXT("1")))
				{
					Scheme.TitleFontName = First->GetString(TEXT("name"));

					const FString Tall = First->GetString(TEXT("tall"));
					if (!Tall.IsEmpty())
					{
						Scheme.TitleHeight = FCString::Atof(*Tall);
					}

					Scheme.TitleWeight = First->GetInt(TEXT("weight"), 0);
					Scheme.bTitleAdditive = First->GetString(TEXT("additive")) == TEXT("1");
				}
			}
		}

		// CustomFontFiles: either "1" "resource/x.ttf", or a section with the file and the name it goes by.
		if (const FSourceKeyValues* CustomFonts = Body.FindChild(TEXT("CustomFontFiles")))
		{
			for (const FSourceKeyValues& Entry : CustomFonts->Children)
			{
				FString File;
				FString Name;

				if (Entry.IsSection())
				{
					File = Entry.GetString(TEXT("font"));
					Name = Entry.GetString(TEXT("name"));
				}
				else
				{
					File = Entry.Value;
				}

				if (File.IsEmpty())
				{
					continue;
				}

				// Without a name of its own the face goes by its file name, which is what the name inside the
				// font almost always is - HALFLIFE2.ttf is the face Source calls "HalfLife2".
				if (Name.IsEmpty())
				{
					Name = FPaths::GetBaseFilename(File);
				}

				Scheme.CustomFontFiles.Add(Name, File);
			}
		}
	}

	UE_LOG(LogLambda, Log, TEXT("Main menu: title '%s' at %g,%g in '%s' at %gpx, %d custom font(s)"),
		*Scheme.Title1, Scheme.Title1Position.X, Scheme.Title1Position.Y,
		Scheme.TitleFontName.IsEmpty() ? TEXT("the engine's own face") : *Scheme.TitleFontName,
		Scheme.TitleHeight, Scheme.CustomFontFiles.Num());

	return Scheme;
}

UFont* FLambdaMenuScheme::GetTitleFont() const
{
	if (!TitleFontName.IsEmpty())
	{
		// A face the scheme ships, by the name the scheme calls it.
		if (const FString* File = CustomFontFiles.Find(TitleFontName))
		{
			if (UFont* Font = FLambdaFonts::Get(FPaths::GetCleanFilename(*File)))
			{
				return Font;
			}
		}

		// The name inside a font and the name of the file holding it are written differently as often as not -
		// "HalfLife2" against HALFLIFE2.ttf - so they are compared with the case and the punctuation taken out.
		const FString Wanted = NormaliseFontName(TitleFontName);
		for (const TPair<FString, FString>& Entry : CustomFontFiles)
		{
			if (NormaliseFontName(Entry.Key) == Wanted || NormaliseFontName(FPaths::GetBaseFilename(Entry.Value)) == Wanted)
			{
				if (UFont* Font = FLambdaFonts::Get(FPaths::GetCleanFilename(Entry.Value)))
				{
					return Font;
				}
			}
		}

		// Last of all, a face sitting in the resource folder under the name asked for, which is how a mod that
		// has not written a CustomFontFiles block can still name one.
		if (UFont* Font = FLambdaFonts::Get(TitleFontName + TEXT(".ttf")))
		{
			return Font;
		}
	}

	return FLambdaFonts::GetTitleFont();
}

const FLambdaGameLogo& FLambdaGameLogo::Get()
{
	static FLambdaGameLogo Logo;
	static bool bLoaded = false;
	if (bLoaded)
	{
		return Logo;
	}
	bLoaded = true;

	// gameinfo.txt's "gamelogo" is what turns it on at all - Source uses the same key to decide whether the
	// menu has a picture or only the two lines of text.
	FSourceKeyValues GameInfo;
	if (!ReadGameInfo(GameInfo) || GameInfo.GetString(TEXT("gamelogo")) != TEXT("1"))
	{
		return Logo;
	}

	FSourceKeyValues Root;
	if (!ReadModFile(TEXT("resource/gamelogo.res"), Root))
	{
		UE_LOG(LogLambda, Log, TEXT("Main menu: gameinfo asks for a logo but there is no resource/gamelogo.res"));
		return Logo;
	}

	const FSourceKeyValues* Panel = Root.FindChild(TEXT("GameLogo"));
	const FSourceKeyValues* Image = Root.FindChild(TEXT("Logo"));
	if (!Panel || !Image)
	{
		return Logo;
	}

	if (Image->GetString(TEXT("visible")) == TEXT("0"))
	{
		return Logo;
	}

	Logo.Offset = FVector2D(Panel->GetFloat(TEXT("offsetX")), Panel->GetFloat(TEXT("offsetY")));
	Logo.PanelSize = FVector2D(Panel->GetFloat(TEXT("wide")), Panel->GetFloat(TEXT("tall")));

	Logo.ImagePosition = FVector2D(Image->GetFloat(TEXT("xpos")), Image->GetFloat(TEXT("ypos")));
	Logo.ImageSize = FVector2D(Image->GetFloat(TEXT("wide")), Image->GetFloat(TEXT("tall")));

	const FString Name = Image->GetString(TEXT("image"));
	if (!Name.IsEmpty())
	{
		Logo.ImageName = Name;
	}

	// Nothing to draw into, or nothing to draw: a logo of no size is no logo.
	if (Logo.PanelSize.X <= 0.0f || Logo.PanelSize.Y <= 0.0f || Logo.ImageSize.X <= 0.0f || Logo.ImageSize.Y <= 0.0f)
	{
		UE_LOG(LogLambda, Warning, TEXT("Main menu: resource/gamelogo.res gives the logo no size"));
		return Logo;
	}

	Logo.bVisible = true;

	UE_LOG(LogLambda, Log, TEXT("Main menu: logo '%s', %gx%g of a %gx%g picture, offset %g,%g"),
		*Logo.ImageName, Logo.PanelSize.X, Logo.PanelSize.Y, Logo.ImageSize.X, Logo.ImageSize.Y,
		Logo.Offset.X, Logo.Offset.Y);

	return Logo;
}

UTexture2D* FLambdaGameLogo::GetTexture() const
{
	if (!bVisible)
	{
		return nullptr;
	}
	// Source keeps the menu's pictures under materials/vgui, and gamelogo.res names this one without that part.
	return FLambdaUITextures::Get(FString::Printf(TEXT("vgui/%s"), *ImageName));
}
