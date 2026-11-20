// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Compare/GameTestCompareRegistry.h"

#include "Internationalization/Regex.h"
#include "Observe/Ref/GameTestPropPath.h"

namespace GameTestCompareTextImpl
{
	using namespace GameTestCompareUtil;

	ESearchCase::Type CaseOf(const FGameTestCompareInput& In)
	{
		return In.bIgnoreCase ? ESearchCase::IgnoreCase : ESearchCase::CaseSensitive;
	}

	FGameTestCompareResult Contains(const FGameTestCompareInput& In)
	{
		return Result(TextOf(In.Actual.Value).Contains(TextOf(In.Expected), CaseOf(In)), In, TEXT("包含"));
	}

	FGameTestCompareResult StartsWith(const FGameTestCompareInput& In)
	{
		return Result(TextOf(In.Actual.Value).StartsWith(TextOf(In.Expected), CaseOf(In)), In, TEXT("开头是"));
	}

	FGameTestCompareResult EndsWith(const FGameTestCompareInput& In)
	{
		return Result(TextOf(In.Actual.Value).EndsWith(TextOf(In.Expected), CaseOf(In)), In, TEXT("结尾是"));
	}

	/** matches：ICU 正则，部分匹配；忽略大小写时加 (?i)。 */
	FGameTestCompareResult Matches(const FGameTestCompareInput& In)
	{
		const FString Pattern = (In.bIgnoreCase ? FString(TEXT("(?i)")) : FString()) + TextOf(In.Expected);
		const FRegexPattern Regex(Pattern);
		FRegexMatcher Matcher(Regex, TextOf(In.Actual.Value));
		return Result(Matcher.FindNext(), In, TEXT("匹配"));
	}

	/** key：value 是本地化键（`命名空间,键` 或只写键），和文字的键比。 */
	FGameTestCompareResult Key(const FGameTestCompareInput& In)
	{
		const FString Expected = TextOf(In.Expected).TrimStartAndEnd();
		const FString& Actual = In.Actual.TextKey;
		FString ActualKey;
		const bool bHasNamespace = Expected.Contains(TEXT(","));
		if (!Actual.Split(TEXT(","), nullptr, &ActualKey))
		{
			ActualKey = Actual;
		}
		const bool bHolds = !Actual.IsEmpty() && (bHasNamespace ? Actual.Equals(Expected) : ActualKey.Equals(Expected));
		FGameTestCompareResult Out;
		Out.bHolds = bHolds;
		if (!bHolds)
		{
			Out.Why = Actual.IsEmpty() ? FString::Printf(TEXT("期望本地化键 %s，实际文字没有键（%s）"), *Expected, *In.Actual.Value.ToDisplay(60))
									   : FString::Printf(TEXT("期望本地化键 %s，实际 %s"), *Expected, *Actual);
		}
		return Out;
	}
}

namespace GameTestBuiltin
{
	void RegisterCompareText()
	{
		using namespace GameTestCompareTextImpl;
		using K = EGameTestKind;
		TGameTestRegistry<IGameTestCompare>& Registry = FGameTestCompareRegistry::Get();
		const FGameTestCompareFn::FValidateFn Text = [](const TSharedPtr<FJsonValue>& V, FGameTestDiagSink& S) { GameTestCompareUtil::ExpectString(V, S); };
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("contains"), TArray<K>{ K::String, K::Text, K::Asset }, TEXT("包含"), TEXT("包含这段文字"), true, &Contains, Text));
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("startsWith"), TArray<K>{ K::String, K::Text }, TEXT("开头是"), TEXT("以这段文字开头"), true, &StartsWith, Text));
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("endsWith"), TArray<K>{ K::String, K::Text }, TEXT("结尾是"), TEXT("以这段文字结尾"), true, &EndsWith, Text));
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("matches"), TArray<K>{ K::String, K::Text }, TEXT("匹配"), TEXT("正则（部分匹配）"), true, &Matches, Text));
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("key"), TArray<K>{ K::Text }, TEXT("的本地化键是"), TEXT("FText 的本地化键（命名空间,键 或只写键）"), true, &Key, Text));
	}
}
