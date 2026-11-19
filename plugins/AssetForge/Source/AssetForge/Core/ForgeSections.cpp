// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ForgeSections.h"
#include "AssetForgeCompat.h"

namespace ForgeSections
{

namespace
{
	/** 头段里固定排在最前的字段，按这个顺序写（`$schema` 是给编辑器的格式指针，和内容无关，放最前不打扰后面的头字段）。 */
	const TCHAR* const FixedHeaderFields[] = {
		TEXT("$schema"),
		TEXT("//"),
		TEXT("kind"),
		TEXT("schemaVersion"),
		TEXT("revision"),
		TEXT("asset"),
		TEXT("parent"),
	};

	bool IsFixedHeaderField(const FString& InKey)
	{
		for (const TCHAR* Field : FixedHeaderFields)
		{
			if (InKey.Equals(Field, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	bool IsBodySection(const FString& InKey, const TArray<FString>& InBodySections)
	{
		return InBodySections.ContainsByPredicate([&InKey](const FString& Section) { return InKey.Equals(Section, ESearchCase::CaseSensitive); });
	}

	TSharedPtr<FJsonValue> ValueOf(const TSharedPtr<FJsonObject>& InObject, const FString& InKey)
	{
		const TSharedPtr<FJsonValue>* Found = InObject->Values.Find(FForgeJsonKey(InKey));
		return Found != nullptr ? *Found : nullptr;
	}

	/** 头段：固定头字段 + 不属于任何大段的其余字段，固定的在前。 */
	TSharedPtr<FJsonObject> HeaderOf(const TSharedPtr<FJsonObject>& InDocument, const TArray<FString>& InBodySections)
	{
		const TSharedPtr<FJsonObject> Header = MakeShared<FJsonObject>();
		for (const TCHAR* Field : FixedHeaderFields)
		{
			if (const TSharedPtr<FJsonValue> Value = ValueOf(InDocument, Field); Value.IsValid())
			{
				Header->SetField(Field, Value);
			}
		}
		for (const auto& Pair : InDocument->Values)
		{
			const FString Key(ASSETFORGE_JSON_KEY(Pair.Key));
			if (!IsFixedHeaderField(Key) && !IsBodySection(Key, InBodySections))
			{
				Header->SetField(Key, Pair.Value);
			}
		}
		return Header;
	}
}

TSharedPtr<FJsonObject> Reorder(const TSharedPtr<FJsonObject>& InDocument, const TArray<FString>& InBodySections)
{
	if (!InDocument.IsValid())
	{
		return nullptr;
	}

	const TSharedPtr<FJsonObject> Out = HeaderOf(InDocument, InBodySections);
	for (const FString& Section : InBodySections)
	{
		if (const TSharedPtr<FJsonValue> Value = ValueOf(InDocument, Section); Value.IsValid())
		{
			Out->SetField(Section, Value);
		}
	}
	return Out;
}

TArray<FString> ListPresent(const TSharedPtr<FJsonObject>& InDocument, const TArray<FString>& InBodySections)
{
	TArray<FString> Out;
	Out.Add(HeaderSection);
	if (!InDocument.IsValid())
	{
		return Out;
	}
	for (const FString& Section : InBodySections)
	{
		if (ValueOf(InDocument, Section).IsValid())
		{
			Out.Add(Section);
		}
	}
	return Out;
}

TSharedPtr<FJsonObject> Extract(const TSharedPtr<FJsonObject>& InDocument, const FString& InSection, const TArray<FString>& InBodySections)
{
	if (!InDocument.IsValid())
	{
		return nullptr;
	}
	if (InSection == HeaderSection)
	{
		return HeaderOf(InDocument, InBodySections);
	}
	if (!IsBodySection(InSection, InBodySections))
	{
		return nullptr;
	}
	const TSharedPtr<FJsonValue> Value = ValueOf(InDocument, InSection);
	if (!Value.IsValid())
	{
		return nullptr;
	}
	const TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetField(InSection, Value);
	return Out;
}

} // namespace ForgeSections
