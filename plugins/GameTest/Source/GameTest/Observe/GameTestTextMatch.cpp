// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestTextMatch.h"

namespace GameTestTextMatchDetail
{
	/** 包含档里短的一方至少几个字：一个字的「确」「取」太容易撞上别的按钮。 */
	constexpr int32 MinContainsChars = 2;

	/** 包含档里短的一方至少占长的多少：「战斗」不该认成「开始战斗副本」。 */
	constexpr double MinContainsRatio = 0.4;

	/** 中文和全角标点、符号：不算进文字（「领取（3）」「剩余：02:15」）。 */
	bool IsWidePunct(TCHAR InChar)
	{
		static const TCHAR* const Marks = TEXT("（）【】「」『』《》〈〉，。、；：！？…—～·．｜／＼＋－×÷％＃＠＆＊“”‘’　");
		for (const TCHAR* Mark = Marks; *Mark != 0; ++Mark)
		{
			if (*Mark == InChar)
			{
				return true;
			}
		}
		return false;
	}

	bool IsDropped(TCHAR InChar)
	{
		if (FChar::IsWhitespace(InChar) || FChar::IsDigit(InChar))
		{
			return true;
		}
		// 全角数字 ０-９
		if (InChar >= 0xFF10 && InChar <= 0xFF19)
		{
			return true;
		}
		// ASCII 里除了字母都去掉（标点、符号）
		if (InChar < 128 && !FChar::IsAlpha(InChar))
		{
			return true;
		}
		return IsWidePunct(InChar);
	}
}

FString GameTestTextMatch::Normalize(const FString& InText)
{
	using namespace GameTestTextMatchDetail;
	FString Out;
	Out.Reserve(InText.Len());
	bool bInTag = false;
	for (const TCHAR Char : InText)
	{
		// 富文本标签 <red>…</>、<img id="x"/>：整段跳过，标签名不算文字
		if (Char == TEXT('<'))
		{
			bInTag = true;
			continue;
		}
		if (bInTag)
		{
			bInTag = Char != TEXT('>');
			continue;
		}
		if (IsDropped(Char))
		{
			continue;
		}
		Out.AppendChar(Char < 128 ? FChar::ToLower(Char) : Char);
	}
	return Out;
}

GameTestTextMatch::ETier GameTestTextMatch::Compare(const FString& InWant, const FString& InNow)
{
	using namespace GameTestTextMatchDetail;
	if (InWant.IsEmpty())
	{
		return ETier::None;
	}
	if (InWant.Equals(InNow))
	{
		return ETier::Exact;
	}
	const FString Want = Normalize(InWant);
	const FString Now = Normalize(InNow);
	if (Want.IsEmpty() || Now.IsEmpty())
	{
		return ETier::None;
	}
	if (Want.Equals(Now))
	{
		return ETier::Normalized;
	}
	const FString& Short = Want.Len() <= Now.Len() ? Want : Now;
	const FString& Long = Want.Len() <= Now.Len() ? Now : Want;
	if (Short.Len() >= MinContainsChars && Short.Len() >= Long.Len() * MinContainsRatio && Long.Contains(Short, ESearchCase::CaseSensitive))
	{
		return ETier::Contains;
	}
	return ETier::None;
}

GameTestTextMatch::ETier GameTestTextMatch::FilterBest(const FString& InWant, const TArray<FString>& InTexts, TArray<int32>& OutIndices)
{
	OutIndices.Reset();
	ETier Best = ETier::None;
	for (int32 Index = 0; Index < InTexts.Num(); ++Index)
	{
		const ETier Tier = Compare(InWant, InTexts[Index]);
		if (Tier == ETier::None || Tier < Best)
		{
			continue;
		}
		if (Tier > Best)
		{
			Best = Tier;
			OutIndices.Reset();
		}
		OutIndices.Add(Index);
	}
	return Best;
}

const TCHAR* GameTestTextMatch::Describe(ETier InTier)
{
	switch (InTier)
	{
	case ETier::Exact:
		return TEXT("文字完全一样");
	case ETier::Normalized:
		return TEXT("忽略数字和标点后文字一样");
	case ETier::Contains:
		return TEXT("文字包含录制时的文字");
	default:
		return TEXT("文字对不上");
	}
}
