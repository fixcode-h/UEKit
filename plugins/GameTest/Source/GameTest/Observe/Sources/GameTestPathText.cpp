// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Sources/GameTestPathText.h"

namespace GameTestPathText
{
bool Parse(const FString& InText, TArray<FGameTestPathSegment>& OutSegments, FString& OutError)
{
	OutSegments.Reset();
	const FString Text = InText.TrimStartAndEnd();
	int32 Index = 0;
	auto ReadName = [&Text, &Index]()
	{
		const int32 Start = Index;
		while (Index < Text.Len() && (FChar::IsAlnum(Text[Index]) || Text[Index] == TEXT('_') || Text[Index] > 0x7F))
		{
			++Index;
		}
		return Text.Mid(Start, Index - Start);
	};

	while (Index < Text.Len())
	{
		const TCHAR Ch = Text[Index];
		if (Ch == TEXT('.'))
		{
			++Index;
			continue;
		}
		if (Ch == TEXT('['))
		{
			const int32 Close = Text.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index);
			if (Close == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("路径 %s 的 [ 没有配对的 ]"), *InText);
				return false;
			}
			const FString Inner = Text.Mid(Index + 1, Close - Index - 1).TrimStartAndEnd();
			if (Inner.Len() >= 2 && (Inner[0] == TEXT('"') || Inner[0] == TEXT('\'')) && Inner[Inner.Len() - 1] == Inner[0])
			{
				OutSegments.Add(FGameTestPathSegment::At(FGameTestValue::FromString(Inner.Mid(1, Inner.Len() - 2))));
			}
			else if (Inner.IsNumeric())
			{
				OutSegments.Add(FGameTestPathSegment::At(FGameTestValue::FromNumber(FCString::Atod(*Inner))));
			}
			else if (!Inner.IsEmpty())
			{
				// 没加引号的键（Tags[Fire]）当字符串：作者常这么写 Map 键
				OutSegments.Add(FGameTestPathSegment::At(FGameTestValue::FromString(Inner)));
			}
			else
			{
				OutError = FString::Printf(TEXT("路径 %s 里有空的 []"), *InText);
				return false;
			}
			Index = Close + 1;
			continue;
		}
		const FString Name = ReadName();
		if (Name.IsEmpty())
		{
			OutError = FString::Printf(TEXT("路径 %s 第 %d 个字 '%c' 认不出（只能写 .成员、[下标]、.函数()）"), *InText, Index + 1, Ch);
			return false;
		}
		if (Index + 1 < Text.Len() && Text[Index] == TEXT('(') && Text[Index + 1] == TEXT(')'))
		{
			Index += 2;
			OutSegments.Add(FGameTestPathSegment::Call(Name, TArray<FGameTestValue>()));
		}
		else if (Index < Text.Len() && Text[Index] == TEXT('('))
		{
			OutError = FString::Printf(TEXT("路径 %s 里的 %s(…) 带了参数：带参数的调用写成表达式（\"=…\"）"), *InText, *Name);
			return false;
		}
		else
		{
			OutSegments.Add(FGameTestPathSegment::Member(Name));
		}
	}
	return true;
}

bool ParseSource(const FString& InText, FName& OutSource, FString& OutSelector, TArray<FGameTestPathSegment>& OutSegments, FString& OutError)
{
	const FString Text = InText.TrimStartAndEnd();
	if (!Text.StartsWith(TEXT("@")))
	{
		OutError = FString::Printf(TEXT("%s 不是来源路径（要以 @ 开头）"), *InText);
		return false;
	}
	int32 Index = 1;
	while (Index < Text.Len() && Text[Index] != TEXT('.') && Text[Index] != TEXT('['))
	{
		++Index;
	}
	const FString Name = Text.Mid(1, Index - 1);
	if (Name.IsEmpty())
	{
		OutError = FString::Printf(TEXT("%s 缺来源名（@gi、@sub、@actor…）"), *InText);
		return false;
	}
	OutSource = FName(*Name);
	OutSelector.Reset();
	// 只有紧跟在来源名后面的 [...] 是选择器（@actor[tag=Boss]）；后面的 [0] 是下标
	if (Index < Text.Len() && Text[Index] == TEXT('['))
	{
		const int32 Close = Text.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index);
		if (Close == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("%s 的 [ 没有配对的 ]"), *InText);
			return false;
		}
		OutSelector = Text.Mid(Index + 1, Close - Index - 1).TrimStartAndEnd();
		Index = Close + 1;
	}
	return Parse(Text.RightChop(Index), OutSegments, OutError);
}

FGameTestValue ResolveSourceText(const FString& InText, FGameTestFlowContext& InContext)
{
	FName Source;
	FString Selector;
	TArray<FGameTestPathSegment> Segments;
	FString Error;
	if (!ParseSource(InText, Source, Selector, Segments, Error))
	{
		return FGameTestValue::Unresolved(Error, TEXT("expr_syntax"));
	}
	return GameTestPath::ResolveSource(Source, Selector, Segments, InContext);
}

FString Join(TArrayView<const FGameTestPathSegment> InSegments)
{
	FString Out;
	for (const FGameTestPathSegment& Segment : InSegments)
	{
		Out += Segment.ToDisplay();
	}
	Out.RemoveFromStart(TEXT("."));
	return Out;
}
}
