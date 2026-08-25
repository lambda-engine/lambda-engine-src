#pragma once

#include "CoreMinimal.h"

/**
 * Tokenizer for Valve KeyValues-style text (VMT, VDF/gameinfo.txt, BSP entity lump).
 * Supports quoted and unquoted tokens, { } blocks, // line comments and [$CONDITIONAL] tokens.
 * Backslashes are kept literally (Valve's default KeyValues behaviour; texture paths use them).
 */
class LAMBDASOURCE_API FSourceKVTokenizer
{
public:
	enum class ETokenType : uint8
	{
		None,
		String,
		OpenBrace,
		CloseBrace,
		End
	};

	struct FToken
	{
		ETokenType Type = ETokenType::None;
		FString Text;
		bool bQuoted = false;
		int32 Line = 0;

		/** Unquoted token of the form [$WIN32] / [!$X360] used for conditional keys. */
		bool IsConditional() const
		{
			return Type == ETokenType::String && !bQuoted && Text.Len() >= 2 && Text[0] == TEXT('[') && Text[Text.Len() - 1] == TEXT(']');
		}
	};

	explicit FSourceKVTokenizer(FStringView InText) : Text(InText) {}

	/** Consumes and returns the next token (Type == End when exhausted). */
	FToken Next();
	/** Returns the next token without consuming it. */
	const FToken& Peek();
	int32 GetLine() const { return Line; }

private:
	FToken ReadToken();
	void SkipWhitespaceAndComments();

	FStringView Text;
	int32 Pos = 0;
	int32 Line = 1;
	bool bHasPeeked = false;
	FToken PeekedToken;
};

/**
 * A node of a parsed KeyValues tree. A node is either a leaf (Key + Value) or a section (Key + Children).
 * Keys are matched case-insensitively, like Valve's KeyValues.
 */
struct LAMBDASOURCE_API FSourceKeyValues
{
	FString Key;
	FString Value;
	TArray<FSourceKeyValues> Children;
	bool bIsSection = false;

	bool IsSection() const { return bIsSection; }

	const FSourceKeyValues* FindChild(FStringView ChildKey) const;
	FSourceKeyValues* FindChild(FStringView ChildKey);
	/** All children with this key (KeyValues allows duplicate keys, e.g. entity outputs). */
	void FindChildren(FStringView ChildKey, TArray<const FSourceKeyValues*>& Out) const;

	FString GetString(FStringView ChildKey, const FString& Default = FString()) const;
	float GetFloat(FStringView ChildKey, float Default = 0.0f) const;
	int32 GetInt(FStringView ChildKey, int32 Default = 0) const;
	bool GetBool(FStringView ChildKey, bool bDefault = false) const;

	/** Parses a whole document. Top-level keys become roots; anonymous top-level { } blocks (entity lump) become roots with an empty Key. */
	static bool ParseText(FStringView Text, TArray<FSourceKeyValues>& OutRoots, FString* OutError = nullptr);
	static bool ParseBytes(TConstArrayView<uint8> Bytes, TArray<FSourceKeyValues>& OutRoots, FString* OutError = nullptr);
	/** Convenience for single-root files such as VMTs: returns the first root. */
	static bool ParseSingle(TConstArrayView<uint8> Bytes, FSourceKeyValues& OutRoot, FString* OutError = nullptr);

	/** Debug dump. */
	FString ToString(int32 Indent = 0) const;

private:
	static bool ParseChildren(FSourceKVTokenizer& Tokenizer, FSourceKeyValues& Parent, bool bIsRoot, FString* OutError);
};
