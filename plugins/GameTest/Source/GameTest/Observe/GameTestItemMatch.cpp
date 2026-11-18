// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestItemMatch.h"
#include "GameTestCompat.h"

#include "Observe/GameTestWidgetNode.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

bool GameTestItemMatch::ValuesEqual(const FJsonValue& InLeft, const FJsonValue& InRight)
{
	if (InLeft.Type == InRight.Type)
	{
		switch (InLeft.Type)
		{
		case EJson::Number:
			return FMath::IsNearlyEqual(InLeft.AsNumber(), InRight.AsNumber(), 0.0001);
		case EJson::Boolean:
			return InLeft.AsBool() == InRight.AsBool();
		default:
			return InLeft.AsString().Equals(InRight.AsString(), ESearchCase::IgnoreCase);
		}
	}

	// 类型不同按文本比：JSON 里 "1" 与 1、true 与 "true" 都算命中。
	return InLeft.AsString().Equals(InRight.AsString(), ESearchCase::IgnoreCase);
}

TSharedPtr<FJsonValue> GameTestItemMatch::FindProperty(const FJsonObject& InProperties, const FString& InPropertyName)
{
	for (const auto& Pair : InProperties.Values)
	{
		// FJsonObject 的键在 5.8 里是 FSharedString，先落成 FString 再比大小写不敏感。
		if (GAMETEST_JSON_KEY(Pair.Key).Equals(InPropertyName, ESearchCase::IgnoreCase))
		{
			return Pair.Value;
		}
	}

	return nullptr;
}

int32 GameTestItemMatch::FindIndexByProperty(const TArray<TSharedPtr<FGameTestListItem>>& InItems, const FString& InProperty, const FJsonValue& InExpected)
{
	for (const TSharedPtr<FGameTestListItem>& Item : InItems)
	{
		if (!Item.IsValid() || !Item->Properties.IsValid())
		{
			continue;
		}

		if (const TSharedPtr<FJsonValue> Value = FindProperty(*Item->Properties, InProperty))
		{
			if (Value.IsValid() && ValuesEqual(*Value, InExpected))
			{
				return Item->Index;
			}
		}
	}

	return INDEX_NONE;
}

FString GameTestItemMatch::DescribeProperty(const TArray<TSharedPtr<FGameTestListItem>>& InItems, const FString& InPropertyName, int32 InLimit)
{
	TArray<FString> Pairs;
	for (int32 Index = 0; Index < InItems.Num() && Index < InLimit; ++Index)
	{
		const TSharedPtr<FGameTestListItem>& Item = InItems[Index];
		if (!Item.IsValid() || !Item->Properties.IsValid())
		{
			continue;
		}

		if (const TSharedPtr<FJsonValue> Value = FindProperty(*Item->Properties, InPropertyName))
		{
			if (Value.IsValid())
			{
				Pairs.Add(FString::Printf(TEXT("[%d] %s=%s"), Item->Index, *InPropertyName, *Value->AsString()));
			}
		}
	}

	if (Pairs.Num() == 0)
	{
		return FString::Printf(TEXT("列表里没有任何一项带属性 '%s'"), *InPropertyName);
	}

	const FString More = InItems.Num() > InLimit
		? FString::Printf(TEXT(" …（共 %d 项）"), InItems.Num())
		: FString();

	return FString::Printf(TEXT("实际值：%s%s"), *FString::Join(Pairs, TEXT("、")), *More);
}
