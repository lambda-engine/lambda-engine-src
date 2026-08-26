#include "Audio/SourceSentences.h"

#include "Core/LambdaSourceModule.h"
#include "FileSystem/LambdaFileSystem.h"

float FSourceSentence::EstimateDuration(TFunctionRef<float(const FString&)> WaveDuration) const
{
	float Total = 0.0f;
	for (const FSourceVoxWord& Word : Words)
	{
		// The window the word is trimmed to, then whatever playback rate the pitch works out to.
		const float Window = FMath::Clamp((Word.End - Word.Start) / 100.0f, 0.0f, 1.0f);
		const float Rate = FMath::Max(0.01f, Word.Pitch / 100.0f);
		Total += WaveDuration(Word.Wave) * Window / Rate;
	}
	return Total;
}

FSourceSentences& FSourceSentences::Get()
{
	static FSourceSentences Instance;
	return Instance;
}

void FSourceSentences::Reset()
{
	Sentences.Reset();
	bInitialized = false;
}

void FSourceSentences::Initialize()
{
	if (bInitialized)
	{
		return;
	}
	bInitialized = true;

	FString Text;
	if (!FLambdaFileSystem::Get().ReadFileToString(TEXT("scripts/sentences.txt"), Text))
	{
		// No sentence file is not an error: a mod that has no speech simply has none.
		UE_LOG(LogLambdaSource, Log, TEXT("sentences: no scripts/sentences.txt found"));
		return;
	}

	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		ParseLine(Line);
	}
	UE_LOG(LogLambdaSource, Log, TEXT("sentences: %d loaded"), Sentences.Num());
}

namespace
{
	/** VOX_ParseWordParams: splits "power_restored(e30)" into its name and the parameters attached to it. */
	void ParseWordParams(const FString& Token, FSourceVoxWord& Word, FString& OutName)
	{
		int32 Open = INDEX_NONE;
		if (!Token.FindChar(TEXT('('), Open) || !Token.EndsWith(TEXT(")")))
		{
			// No formatting: the whole token is the word.
			OutName = Token;
			return;
		}
		OutName = Token.Left(Open);

		const FString Params = Token.Mid(Open + 1, Token.Len() - Open - 2);
		for (int32 i = 0; i < Params.Len(); ++i)
		{
			const TCHAR Command = Params[i];
			if (Command != TEXT('v') && Command != TEXT('p') && Command != TEXT('s')
				&& Command != TEXT('e') && Command != TEXT('t'))
			{
				continue;
			}
			int32 j = i + 1;
			FString Digits;
			while (j < Params.Len() && FChar::IsDigit(Params[j]))
			{
				Digits.AppendChar(Params[j++]);
			}
			if (Digits.IsEmpty())
			{
				continue;
			}
			const int32 Value = FCString::Atoi(*Digits);
			switch (Command)
			{
			case TEXT('v'): Word.Volume = Value; break;
			case TEXT('p'): Word.Pitch = Value; break;
			case TEXT('s'): Word.Start = Value; break;
			case TEXT('e'): Word.End = Value; break;
			case TEXT('t'): Word.TimeCompress = Value; break;
			default: break;
			}
			i = j - 1;
		}
	}
}

void FSourceSentences::ParseLine(const FString& RawLine)
{
	FString Line = RawLine;

	// Black Mesa's {Len 3.67 closecaption HEV.minor_fracture} tail, which Valve's file does not have.
	int32 Brace = INDEX_NONE;
	if (Line.FindChar(TEXT('{'), Brace))
	{
		Line.LeftInline(Brace);
	}
	Line.TrimStartAndEndInline();
	if (Line.IsEmpty() || Line.StartsWith(TEXT("//")))
	{
		return;
	}

	// "NAME word word word" - the name runs to the first space or tab.
	int32 Split = INDEX_NONE;
	for (int32 i = 0; i < Line.Len(); ++i)
	{
		if (FChar::IsWhitespace(Line[i]))
		{
			Split = i;
			break;
		}
	}
	if (Split == INDEX_NONE)
	{
		return;
	}
	FSourceSentence Sentence;
	Sentence.Name = Line.Left(Split).ToUpper();

	FString Body = Line.Mid(Split + 1);
	// Commas separate words and mean nothing else, so they may as well be spaces.
	Body.ReplaceInline(TEXT(","), TEXT(" "));

	TArray<FString> Tokens;
	Body.ParseIntoArrayWS(Tokens);

	// The directory the first word carries applies to every word after it (VOX_GetDirectory).
	FString Directory;
	FSourceVoxWord Defaults;

	for (const FString& Token : Tokens)
	{
		FSourceVoxWord Word = Defaults;
		FString Name;
		ParseWordParams(Token, Word, Name);

		// The directory comes off first - "hev_vox/(p160)" is a directory and a parameter block, not a word.
		int32 Slash = INDEX_NONE;
		if (Name.FindLastChar(TEXT('/'), Slash))
		{
			Directory = Name.Left(Slash + 1);
			Name = Name.Mid(Slash + 1);
		}
		if (Name.IsEmpty())
		{
			// Nothing left to play, so this was an isolated parameter block: everything after it inherits it.
			Defaults = Word;
			continue;
		}
		Word.Wave = Directory + Name;
		if (Word.Pitch <= 0)
		{
			Word.Pitch = 100;	// voxword_t starts pitch at -1 for "as recorded"
		}
		Sentence.Words.Add(MoveTemp(Word));
	}

	if (Sentence.Words.Num() > 0)
	{
		Sentences.Add(Sentence.Name, MoveTemp(Sentence));
	}
}

const FSourceSentence* FSourceSentences::Find(const FString& Name)
{
	Initialize();
	FString Key = Name;
	Key.RemoveFromStart(TEXT("!"));	// the game code writes "!HEV_DMG4"
	return Sentences.Find(Key.ToUpper());
}
