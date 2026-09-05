#pragma once

#include "CoreMinimal.h"

/**
 * DmAttributeType_t (public/datamodel/dmattributetypes.h). Array types are the value type plus 14.
 */
enum class ESourceDMXType : uint8
{
	Unknown = 0,
	Element = 1,
	Int,
	Float,
	Bool,
	String,
	Void,
	Time,
	Color,
	Vector2,
	Vector3,
	Vector4,
	QAngle,
	Quaternion,
	VMatrix,
	ElementArray = 15,
	IntArray,
	FloatArray,
	BoolArray,
	StringArray,
	VoidArray,
	TimeArray,
	ColorArray,
	Vector2Array,
	Vector3Array,
	Vector4Array,
	QAngleArray,
	QuaternionArray,
	VMatrixArray,
};

/** One attribute value. Scalars share the Number field; vectors, angles and colours share Vector. */
struct LAMBDASOURCE_API FSourceDMXValue
{
	ESourceDMXType Type = ESourceDMXType::Unknown;
	float Number = 0.0f;						// Int, Float, Bool, Time (seconds)
	FString String;								// String
	FVector4f Vector = FVector4f::Zero();		// Vector2/3/4, QAngle, Quaternion; Color as 0..255 per component
	int32 Element = INDEX_NONE;					// Element: index into the file's element list, INDEX_NONE for null
	TArray<FSourceDMXValue> Array;				// *Array types: one entry per item, each of the item type

	bool IsArray() const { return Type >= ESourceDMXType::ElementArray; }
	bool IsNumber() const
	{
		return Type == ESourceDMXType::Int || Type == ESourceDMXType::Float || Type == ESourceDMXType::Bool || Type == ESourceDMXType::Time;
	}
};

/** An element: a typed, named object with attributes. The "name" attribute lives in the dictionary, not the list. */
struct LAMBDASOURCE_API FSourceDMXElement
{
	FString Type;
	FString Name;
	uint8 Id[16] = {};
	TArray<TPair<FString, FSourceDMXValue>> Attributes;

	/** Case-insensitive attribute lookup, or null. */
	const FSourceDMXValue* Find(FStringView Key) const;
	bool Has(FStringView Key) const { return Find(Key) != nullptr; }

	/** Typed reads with the caller's default when the attribute is absent or of another kind. Numbers convert freely. */
	float GetFloat(FStringView Key, float Default = 0.0f) const;
	int32 GetInt(FStringView Key, int32 Default = 0) const;
	bool GetBool(FStringView Key, bool bDefault = false) const;
	FString GetString(FStringView Key, const FString& Default = FString()) const;
	FVector3f GetVector3(FStringView Key, const FVector3f& Default = FVector3f::ZeroVector) const;
	FVector4f GetVector4(FStringView Key, const FVector4f& Default = FVector4f::Zero()) const;
	FColor GetColor(FStringView Key, const FColor& Default = FColor::White) const;
	/** The element an Element attribute refers to, INDEX_NONE when absent or null. */
	int32 GetElementIndex(FStringView Key) const;
	/** The element indices of an ElementArray attribute (nulls skipped). */
	void GetElementIndices(FStringView Key, TArray<int32>& Out) const;
};

/**
 * Reader for Valve's binary DMX container (dmxloader/dmxloader.cpp), which is what a particle .pcf is.
 *
 * The layout is a header line, a NUL, a string table (encoding 2 and up), an element dictionary of (type, name,
 * GUID) and then one attribute list per element in dictionary order. The five binary encodings differ only in
 * whether names come from the string table and how wide the table indices are; all five are read. Element 0 is
 * the root.
 */
class LAMBDASOURCE_API FSourceDMXFile
{
public:
	bool Load(TConstArrayView<uint8> Data, FString* OutError = nullptr);

	bool IsLoaded() const { return Elements.Num() > 0; }
	const FSourceDMXElement* GetRoot() const { return Elements.Num() > 0 ? &Elements[0] : nullptr; }
	const FSourceDMXElement* GetElement(int32 Index) const { return Elements.IsValidIndex(Index) ? &Elements[Index] : nullptr; }
	const TArray<FSourceDMXElement>& GetElements() const { return Elements; }
	int32 GetEncodingVersion() const { return EncodingVersion; }
	const FString& GetFormatName() const { return FormatName; }

private:
	class FCursor;
	bool ReadValue(FCursor& Cursor, ESourceDMXType Type, FSourceDMXValue& Out, FString* OutError) const;

	TArray<FSourceDMXElement> Elements;
	TArray<FString> Strings;
	int32 EncodingVersion = 0;
	FString FormatName;
};
