#include "SourceKeyValues.h"
#include "LambdaSourceModule.h"
#include "Misc/FileHelper.h"

// ---------------------------------------------------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------------------------------------------------

void FSourceKVTokenizer::SkipWhitespaceAndComments()
{
	const int32 Len = Text.Len();
	while (Pos < Len)
	{
		const TCHAR C = Text[Pos];
		if (C == TEXT('\n'))
		{
			++Line;
			++Pos;
		}
		else if (FChar::IsWhitespace(C) || C == TEXT('\r'))
		{
			++Pos;
		}
		else if (C == TEXT('/') && Pos + 1 < Len && Text[Pos + 1] == TEXT('/'))
		{
			// line comment
			while (Pos < Len && Text[Pos] != TEXT('\n'))
			{
				++Pos;
			}
		}
		else if (C == TEXT('/') && Pos + 1 < Len && Text[Pos + 1] == TEXT('*'))
		{
			// block comment (not standard KeyValues but harmless to support)
			Pos += 2;
			while (Pos + 1 < Len && !(Text[Pos] == TEXT('*') && Text[Pos + 1] == TEXT('/')))
			{
				if (Text[Pos] == TEXT('\n'))
				{
					++Line;
				}
				++Pos;
			}
			Pos = FMath::Min(Pos + 2, Len);
		}
		else
		{
			break;
		}
	}
}

FSourceKVTokenizer::FToken FSourceKVTokenizer::ReadToken()
{
	FToken Token;
	SkipWhitespaceAndComments();

	const int32 Len = Text.Len();
	Token.Line = Line;
	if (Pos >= Len)
	{
		Token.Type = ETokenType::End;
		return Token;
	}

	const TCHAR C = Text[Pos];
	if (C == TEXT('{'))
	{
		++Pos;
		Token.Type = ETokenType::OpenBrace;
		return Token;
	}
	if (C == TEXT('}'))
	{
		++Pos;
		Token.Type = ETokenType::CloseBrace;
		return Token;
	}

	Token.Type = ETokenType::String;
	if (C == TEXT('"'))
	{
		// Quoted string: read literally up to the closing quote (no escape processing, like Valve's default KeyValues).
		++Pos;
		const int32 Start = Pos;
		while (Pos < Len && Text[Pos] != TEXT('"'))
		{
			if (Text[Pos] == TEXT('\n'))
			{
				++Line;
			}
			++Pos;
		}
		Token.Text = FString(Text.Mid(Start, Pos - Start));
		Token.bQuoted = true;
		if (Pos < Len)
		{
			++Pos; // closing quote
		}
		return Token;
	}

	if (C == TEXT('['))
	{
		// Conditional: read up to and including ']'
		const int32 Start = Pos;
		while (Pos < Len && Text[Pos] != TEXT(']') && Text[Pos] != TEXT('\n'))
		{
			++Pos;
		}
		if (Pos < Len && Text[Pos] == TEXT(']'))
		{
			++Pos;
		}
		Token.Text = FString(Text.Mid(Start, Pos - Start));
		return Token;
	}

	// Unquoted token: up to whitespace, brace, quote or comment start.
	const int32 Start = Pos;
	while (Pos < Len)
	{
		const TCHAR D = Text[Pos];
		if (FChar::IsWhitespace(D) || D == TEXT('{') || D == TEXT('}') || D == TEXT('"'))
		{
			break;
		}
		if (D == TEXT('/') && Pos + 1 < Len && (Text[Pos + 1] == TEXT('/') || Text[Pos + 1] == TEXT('*')))
		{
			break;
		}
		++Pos;
	}
	Token.Text = FString(Text.Mid(Start, Pos - Start));
	return Token;
}

FSourceKVTokenizer::FToken FSourceKVTokenizer::Next()
{
	if (bHasPeeked)
	{
		bHasPeeked = false;
		return MoveTemp(PeekedToken);
	}
	return ReadToken();
}

const FSourceKVTokenizer::FToken& FSourceKVTokenizer::Peek()
{
	if (!bHasPeeked)
	{
		PeekedToken = ReadToken();
		bHasPeeked = true;
	}
	return PeekedToken;
}

// ---------------------------------------------------------------------------------------------------------------------
// FSourceKeyValues
// ---------------------------------------------------------------------------------------------------------------------

const FSourceKeyValues* FSourceKeyValues::FindChild(FStringView ChildKey) const
{
	for (const FSourceKeyValues& Child : Children)
	{
		if (ChildKey.Equals(Child.Key, ESearchCase::IgnoreCase))
		{
			return &Child;
		}
	}
	return nullptr;
}

FSourceKeyValues* FSourceKeyValues::FindChild(FStringView ChildKey)
{
	for (FSourceKeyValues& Child : Children)
	{
		if (ChildKey.Equals(Child.Key, ESearchCase::IgnoreCase))
		{
			return &Child;
		}
	}
	return nullptr;
}

void FSourceKeyValues::FindChildren(FStringView ChildKey, TArray<const FSourceKeyValues*>& Out) const
{
	for (const FSourceKeyValues& Child : Children)
	{
		if (ChildKey.Equals(Child.Key, ESearchCase::IgnoreCase))
		{
			Out.Add(&Child);
		}
	}
}

FString FSourceKeyValues::GetString(FStringView ChildKey, const FString& Default) const
{
	const FSourceKeyValues* Child = FindChild(ChildKey);
	return Child ? Child->Value : Default;
}

float FSourceKeyValues::GetFloat(FStringView ChildKey, float Default) const
{
	const FSourceKeyValues* Child = FindChild(ChildKey);
	if (Child && Child->Value.IsNumeric())
	{
		return FCString::Atof(*Child->Value);
	}
	return Default;
}

int32 FSourceKeyValues::GetInt(FStringView ChildKey, int32 Default) const
{
	const FSourceKeyValues* Child = FindChild(ChildKey);
	if (Child && Child->Value.IsNumeric())
	{
		return FCString::Atoi(*Child->Value);
	}
	return Default;
}

bool FSourceKeyValues::GetBool(FStringView ChildKey, bool bDefault) const
{
	const FSourceKeyValues* Child = FindChild(ChildKey);
	if (!Child)
	{
		return bDefault;
	}
	if (Child->Value.IsNumeric())
	{
		return FCString::Atoi(*Child->Value) != 0;
	}
	return Child->Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Child->Value.Equals(TEXT("yes"), ESearchCase::IgnoreCase);
}

bool FSourceKeyValues::ParseChildren(FSourceKVTokenizer& Tokenizer, FSourceKeyValues& Parent, bool bIsRoot, FString* OutError)
{
	using ETokenType = FSourceKVTokenizer::ETokenType;

	for (;;)
	{
		FSourceKVTokenizer::FToken Token = Tokenizer.Next();
		switch (Token.Type)
		{
		case ETokenType::End:
			// Be lenient with unterminated blocks; Valve's parser is too.
			return true;

		case ETokenType::CloseBrace:
			if (bIsRoot)
			{
				// stray '}' at root level - ignore
				continue;
			}
			return true;

		case ETokenType::OpenBrace:
		{
			// Anonymous block (entity lump style)
			FSourceKeyValues Child;
			Child.bIsSection = true;
			if (!ParseChildren(Tokenizer, Child, false, OutError))
			{
				return false;
			}
			Parent.Children.Add(MoveTemp(Child));
			continue;
		}

		case ETokenType::String:
		{
			if (Token.IsConditional())
			{
				continue; // stray conditional
			}
			if (!Token.bQuoted && Token.Text.Len() > 0 && Token.Text[0] == TEXT('#'))
			{
				// #include / #base "file" - not supported, skip the key and its value
				const FSourceKVTokenizer::FToken& Next = Tokenizer.Peek();
				if (Next.Type == ETokenType::String)
				{
					Tokenizer.Next();
				}
				continue;
			}

			FSourceKeyValues Child;
			Child.Key = MoveTemp(Token.Text);

			// Optional conditional between key and value/block
			while (Tokenizer.Peek().IsConditional())
			{
				Tokenizer.Next();
			}

			const FSourceKVTokenizer::FToken& Next = Tokenizer.Peek();
			if (Next.Type == ETokenType::OpenBrace)
			{
				Tokenizer.Next();
				Child.bIsSection = true;
				if (!ParseChildren(Tokenizer, Child, false, OutError))
				{
					return false;
				}
			}
			else if (Next.Type == ETokenType::String)
			{
				FSourceKVTokenizer::FToken ValueToken = Tokenizer.Next();
				Child.Value = MoveTemp(ValueToken.Text);
				// Optional trailing conditional
				while (Tokenizer.Peek().IsConditional())
				{
					Tokenizer.Next();
				}
			}
			else
			{
				// key without value (lenient): keep as empty leaf
			}
			Parent.Children.Add(MoveTemp(Child));
			continue;
		}

		default:
			if (OutError)
			{
				*OutError = FString::Printf(TEXT("Unexpected token at line %d"), Token.Line);
			}
			return false;
		}
	}
}

bool FSourceKeyValues::ParseText(FStringView Text, TArray<FSourceKeyValues>& OutRoots, FString* OutError)
{
	FSourceKVTokenizer Tokenizer(Text);
	FSourceKeyValues Root;
	Root.bIsSection = true;
	if (!ParseChildren(Tokenizer, Root, true, OutError))
	{
		return false;
	}
	OutRoots = MoveTemp(Root.Children);
	return true;
}

bool FSourceKeyValues::ParseBytes(TConstArrayView<uint8> Bytes, TArray<FSourceKeyValues>& OutRoots, FString* OutError)
{
	FString Text;
	FFileHelper::BufferToString(Text, Bytes.GetData(), Bytes.Num());
	return ParseText(Text, OutRoots, OutError);
}

bool FSourceKeyValues::ParseSingle(TConstArrayView<uint8> Bytes, FSourceKeyValues& OutRoot, FString* OutError)
{
	TArray<FSourceKeyValues> Roots;
	if (!ParseBytes(Bytes, Roots, OutError))
	{
		return false;
	}
	if (Roots.Num() == 0)
	{
		if (OutError)
		{
			*OutError = TEXT("Document contains no keys");
		}
		return false;
	}
	OutRoot = MoveTemp(Roots[0]);
	return true;
}

FString FSourceKeyValues::ToString(int32 Indent) const
{
	FString Pad;
	for (int32 i = 0; i < Indent; ++i)
	{
		Pad += TEXT("\t");
	}
	FString Result;
	if (bIsSection)
	{
		Result += FString::Printf(TEXT("%s\"%s\"\n%s{\n"), *Pad, *Key, *Pad);
		for (const FSourceKeyValues& Child : Children)
		{
			Result += Child.ToString(Indent + 1);
		}
		Result += FString::Printf(TEXT("%s}\n"), *Pad);
	}
	else
	{
		Result += FString::Printf(TEXT("%s\"%s\" \"%s\"\n"), *Pad, *Key, *Value);
	}
	return Result;
}
