#include "Formats/SourceDMXFile.h"
#include "Core/LambdaSourceModule.h"

// ---- FSourceDMXElement ----------------------------------------------------------------------------------------

const FSourceDMXValue* FSourceDMXElement::Find(FStringView Key) const
{
	for (const TPair<FString, FSourceDMXValue>& Pair : Attributes)
	{
		if (Pair.Key.Equals(FString(Key), ESearchCase::IgnoreCase))
		{
			return &Pair.Value;
		}
	}
	return nullptr;
}

float FSourceDMXElement::GetFloat(FStringView Key, float Default) const
{
	const FSourceDMXValue* Value = Find(Key);
	return (Value && Value->IsNumber()) ? Value->Number : Default;
}

int32 FSourceDMXElement::GetInt(FStringView Key, int32 Default) const
{
	const FSourceDMXValue* Value = Find(Key);
	return (Value && Value->IsNumber()) ? (int32)Value->Number : Default;
}

bool FSourceDMXElement::GetBool(FStringView Key, bool bDefault) const
{
	const FSourceDMXValue* Value = Find(Key);
	return (Value && Value->IsNumber()) ? Value->Number != 0.0f : bDefault;
}

FString FSourceDMXElement::GetString(FStringView Key, const FString& Default) const
{
	const FSourceDMXValue* Value = Find(Key);
	return (Value && Value->Type == ESourceDMXType::String) ? Value->String : Default;
}

FVector3f FSourceDMXElement::GetVector3(FStringView Key, const FVector3f& Default) const
{
	const FSourceDMXValue* Value = Find(Key);
	if (Value && (Value->Type == ESourceDMXType::Vector3 || Value->Type == ESourceDMXType::QAngle
		|| Value->Type == ESourceDMXType::Vector4 || Value->Type == ESourceDMXType::Vector2))
	{
		return FVector3f(Value->Vector.X, Value->Vector.Y, Value->Vector.Z);
	}
	return Default;
}

FVector4f FSourceDMXElement::GetVector4(FStringView Key, const FVector4f& Default) const
{
	const FSourceDMXValue* Value = Find(Key);
	if (Value && (Value->Type == ESourceDMXType::Vector4 || Value->Type == ESourceDMXType::Quaternion
		|| Value->Type == ESourceDMXType::Vector3 || Value->Type == ESourceDMXType::Color))
	{
		return Value->Vector;
	}
	return Default;
}

FColor FSourceDMXElement::GetColor(FStringView Key, const FColor& Default) const
{
	const FSourceDMXValue* Value = Find(Key);
	if (Value && Value->Type == ESourceDMXType::Color)
	{
		return FColor((uint8)FMath::Clamp(Value->Vector.X, 0.0f, 255.0f), (uint8)FMath::Clamp(Value->Vector.Y, 0.0f, 255.0f),
			(uint8)FMath::Clamp(Value->Vector.Z, 0.0f, 255.0f), (uint8)FMath::Clamp(Value->Vector.W, 0.0f, 255.0f));
	}
	return Default;
}

int32 FSourceDMXElement::GetElementIndex(FStringView Key) const
{
	const FSourceDMXValue* Value = Find(Key);
	return (Value && Value->Type == ESourceDMXType::Element) ? Value->Element : INDEX_NONE;
}

void FSourceDMXElement::GetElementIndices(FStringView Key, TArray<int32>& Out) const
{
	const FSourceDMXValue* Value = Find(Key);
	if (!Value || Value->Type != ESourceDMXType::ElementArray)
	{
		return;
	}
	for (const FSourceDMXValue& Item : Value->Array)
	{
		if (Item.Element != INDEX_NONE)
		{
			Out.Add(Item.Element);
		}
	}
}

// ---- FSourceDMXFile -------------------------------------------------------------------------------------------

/** Bounds-checked little-endian reads; a read past the end flags the cursor and returns zeros. */
class FSourceDMXFile::FCursor
{
public:
	explicit FCursor(TConstArrayView<uint8> InData) : Data(InData) {}

	bool IsOk() const { return !bFailed; }
	int64 Tell() const { return Pos; }

	template <typename T>
	T Read()
	{
		T Value{};
		if (Pos + (int64)sizeof(T) > Data.Num())
		{
			bFailed = true;
			Pos = Data.Num();
			return Value;
		}
		FMemory::Memcpy(&Value, Data.GetData() + Pos, sizeof(T));
		Pos += sizeof(T);
		return Value;
	}

	FString ReadCString()
	{
		const int64 Start = Pos;
		while (Pos < Data.Num() && Data[Pos] != 0)
		{
			++Pos;
		}
		if (Pos >= Data.Num())
		{
			bFailed = true;
			return FString();
		}
		FUTF8ToTCHAR Converter((const ANSICHAR*)Data.GetData() + Start, (int32)(Pos - Start));
		++Pos;	// the NUL
		return FString(Converter.Length(), Converter.Get());
	}

	bool Skip(int64 Bytes)
	{
		if (Pos + Bytes > Data.Num())
		{
			bFailed = true;
			Pos = Data.Num();
			return false;
		}
		Pos += Bytes;
		return true;
	}

private:
	TConstArrayView<uint8> Data;
	int64 Pos = 0;
	bool bFailed = false;
};

namespace
{
	bool Fail(FString* OutError, const FString& Message)
	{
		if (OutError)
		{
			*OutError = Message;
		}
		return false;
	}
}

bool FSourceDMXFile::Load(TConstArrayView<uint8> Data, FString* OutError)
{
	Elements.Reset();
	Strings.Reset();
	EncodingVersion = 0;

	// The header is a comment line ended by a NUL: "<!-- dmx encoding binary 2 format pcf 1 -->\n\0".
	int32 HeaderEnd = INDEX_NONE;
	for (int32 i = 0; i < Data.Num() && i < 256; ++i)
	{
		if (Data[i] == 0)
		{
			HeaderEnd = i;
			break;
		}
	}
	if (HeaderEnd == INDEX_NONE)
	{
		return Fail(OutError, TEXT("no DMX header"));
	}
	FString Header;
	{
		FUTF8ToTCHAR Converter((const ANSICHAR*)Data.GetData(), HeaderEnd);
		Header = FString(Converter.Length(), Converter.Get());
	}

	FString EncodingName;
	{
		// "<!-- dmx encoding <name> <version> format <name> <version> -->", or the older "<!-- DMXVersion binary_v2 -->".
		TArray<FString> Tokens;
		Header.ParseIntoArrayWS(Tokens);
		for (int32 i = 0; i + 1 < Tokens.Num(); ++i)
		{
			if (Tokens[i].Equals(TEXT("encoding"), ESearchCase::IgnoreCase) && i + 2 < Tokens.Num())
			{
				EncodingName = Tokens[i + 1];
				EncodingVersion = FCString::Atoi(*Tokens[i + 2]);
			}
			else if (Tokens[i].Equals(TEXT("format"), ESearchCase::IgnoreCase))
			{
				FormatName = Tokens[i + 1];
			}
			else if (Tokens[i].Equals(TEXT("DMXVersion"), ESearchCase::IgnoreCase))
			{
				const FString& Legacy = Tokens[i + 1];
				if (Legacy.StartsWith(TEXT("binary_v")))
				{
					EncodingName = TEXT("binary");
					EncodingVersion = FCString::Atoi(*Legacy.RightChop(8));
				}
				else
				{
					EncodingName = Legacy;
				}
			}
		}
	}
	if (!EncodingName.Equals(TEXT("binary"), ESearchCase::IgnoreCase))
	{
		return Fail(OutError, FString::Printf(TEXT("DMX encoding '%s' is not supported (only binary)"), *EncodingName));
	}
	if (EncodingVersion < 1 || EncodingVersion > 5)
	{
		return Fail(OutError, FString::Printf(TEXT("DMX binary encoding %d is not supported"), EncodingVersion));
	}

	FCursor Cursor(Data.Slice(HeaderEnd + 1, Data.Num() - HeaderEnd - 1));
	const int32 Version = EncodingVersion;

	// String table: encoding 2 and up; a 16-bit count until encoding 4, 32-bit after.
	if (Version >= 2)
	{
		const int32 NumStrings = (Version >= 4) ? Cursor.Read<int32>() : (int32)Cursor.Read<int16>();
		if (NumStrings < 0 || NumStrings > 10 * 1024 * 1024)
		{
			return Fail(OutError, TEXT("DMX string table count is nonsense"));
		}
		Strings.Reserve(NumStrings);
		for (int32 i = 0; i < NumStrings && Cursor.IsOk(); ++i)
		{
			Strings.Add(Cursor.ReadCString());
		}
	}

	// Table indices are 16-bit until encoding 5.
	auto ReadIndex = [&Cursor, Version]() -> int32
	{
		return (Version >= 5) ? Cursor.Read<int32>() : (int32)Cursor.Read<uint16>();
	};
	auto TableString = [this](int32 Index) -> FString
	{
		return Strings.IsValidIndex(Index) ? Strings[Index] : FString();
	};

	const int32 NumElements = Cursor.Read<int32>();
	if (!Cursor.IsOk() || NumElements < 0 || NumElements > 4 * 1024 * 1024)
	{
		return Fail(OutError, TEXT("DMX element count is nonsense"));
	}
	Elements.SetNum(NumElements);
	for (int32 i = 0; i < NumElements && Cursor.IsOk(); ++i)
	{
		FSourceDMXElement& Element = Elements[i];
		Element.Type = (Version >= 2) ? TableString(ReadIndex()) : Cursor.ReadCString();
		Element.Name = (Version >= 4) ? TableString(ReadIndex()) : Cursor.ReadCString();
		for (int32 b = 0; b < 16; ++b)
		{
			Element.Id[b] = Cursor.Read<uint8>();
		}
	}
	if (!Cursor.IsOk())
	{
		return Fail(OutError, TEXT("DMX element dictionary is truncated"));
	}

	for (int32 i = 0; i < NumElements; ++i)
	{
		FSourceDMXElement& Element = Elements[i];
		const int32 NumAttributes = Cursor.Read<int32>();
		if (!Cursor.IsOk() || NumAttributes < 0 || NumAttributes > 1024 * 1024)
		{
			return Fail(OutError, FString::Printf(TEXT("DMX element %d ('%s') has a nonsense attribute count"), i, *Element.Name));
		}
		Element.Attributes.Reserve(NumAttributes);
		for (int32 a = 0; a < NumAttributes; ++a)
		{
			FString AttrName = (Version >= 2) ? TableString(ReadIndex()) : Cursor.ReadCString();
			const ESourceDMXType Type = (ESourceDMXType)Cursor.Read<uint8>();
			FSourceDMXValue Value;
			if (!ReadValue(Cursor, Type, Value, OutError))
			{
				return false;
			}
			Element.Attributes.Emplace(MoveTemp(AttrName), MoveTemp(Value));
		}
		if (!Cursor.IsOk())
		{
			return Fail(OutError, FString::Printf(TEXT("DMX element %d ('%s') is truncated"), i, *Element.Name));
		}
	}
	return true;
}

bool FSourceDMXFile::ReadValue(FCursor& Cursor, ESourceDMXType Type, FSourceDMXValue& Out, FString* OutError) const
{
	Out.Type = Type;
	if (Type >= ESourceDMXType::ElementArray)
	{
		const int32 Count = Cursor.Read<int32>();
		if (!Cursor.IsOk() || Count < 0 || Count > 16 * 1024 * 1024)
		{
			return Fail(OutError, TEXT("DMX array count is nonsense"));
		}
		const ESourceDMXType Inner = (ESourceDMXType)((uint8)Type - 14);
		Out.Array.Reserve(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			FSourceDMXValue Item;
			if (Inner == ESourceDMXType::String)
			{
				// Strings inside an array are always inline, whatever the encoding (the table shortcut is scalar-only).
				Item.Type = Inner;
				Item.String = Cursor.ReadCString();
			}
			else if (!ReadValue(Cursor, Inner, Item, OutError))
			{
				return false;
			}
			Out.Array.Add(MoveTemp(Item));
		}
		return Cursor.IsOk();
	}

	switch (Type)
	{
	case ESourceDMXType::Element:
	{
		const int32 Index = Cursor.Read<int32>();
		if (Index == -2)
		{
			// An external reference: a GUID string follows. Nothing here can resolve it, so it reads as null.
			Cursor.ReadCString();
			Out.Element = INDEX_NONE;
		}
		else
		{
			Out.Element = (Index >= 0) ? Index : INDEX_NONE;
		}
		break;
	}
	case ESourceDMXType::Int:
		Out.Number = (float)Cursor.Read<int32>();
		break;
	case ESourceDMXType::Float:
		Out.Number = Cursor.Read<float>();
		break;
	case ESourceDMXType::Bool:
		Out.Number = Cursor.Read<uint8>() ? 1.0f : 0.0f;
		break;
	case ESourceDMXType::String:
		if (EncodingVersion >= 4)
		{
			const int32 Index = (EncodingVersion >= 5) ? Cursor.Read<int32>() : (int32)Cursor.Read<uint16>();
			Out.String = Strings.IsValidIndex(Index) ? Strings[Index] : FString();
		}
		else
		{
			Out.String = Cursor.ReadCString();
		}
		break;
	case ESourceDMXType::Void:
	{
		const int32 Length = Cursor.Read<int32>();
		if (Length < 0 || !Cursor.Skip(Length))
		{
			return Fail(OutError, TEXT("DMX binary blob is truncated"));
		}
		break;
	}
	case ESourceDMXType::Time:
		Out.Number = Cursor.Read<int32>() / 10000.0f;
		break;
	case ESourceDMXType::Color:
	{
		const uint8 R = Cursor.Read<uint8>(), G = Cursor.Read<uint8>(), B = Cursor.Read<uint8>(), A = Cursor.Read<uint8>();
		Out.Vector = FVector4f(R, G, B, A);
		break;
	}
	case ESourceDMXType::Vector2:
		Out.Vector = FVector4f(Cursor.Read<float>(), Cursor.Read<float>(), 0.0f, 0.0f);
		break;
	case ESourceDMXType::Vector3:
	case ESourceDMXType::QAngle:
		Out.Vector = FVector4f(Cursor.Read<float>(), Cursor.Read<float>(), Cursor.Read<float>(), 0.0f);
		break;
	case ESourceDMXType::Vector4:
	case ESourceDMXType::Quaternion:
		Out.Vector = FVector4f(Cursor.Read<float>(), Cursor.Read<float>(), Cursor.Read<float>(), Cursor.Read<float>());
		break;
	case ESourceDMXType::VMatrix:
		Cursor.Skip(64);
		break;
	default:
		return Fail(OutError, FString::Printf(TEXT("DMX attribute type %d is unknown (at byte %lld)"), (int32)Type, Cursor.Tell()));
	}
	return Cursor.IsOk();
}
