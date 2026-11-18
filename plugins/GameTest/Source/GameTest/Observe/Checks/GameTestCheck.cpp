// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Checks/GameTestCheck.h"

#include "Core/Script/GameTestBuiltins.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestRefs.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Regex.h"

TGameTestRegistry<IGameTestCheck>& GameTestChecks::Get()
{
	static TGameTestRegistry<IGameTestCheck> Registry;
	GameTestBuiltins::EnsureRegistered();
	return Registry;
}

const IGameTestCheck* GameTestChecks::FindFor(const FJsonObject& InJson)
{
	for (const TSharedRef<IGameTestCheck>& Check : Get().GetAll())
	{
		if (Check->Matches(InJson))
		{
			return &Check.Get();
		}
	}
	return nullptr;
}

namespace GameTestCompare
{
	const TArray<FString>& Keys()
	{
		static const TArray<FString> List = {
			TEXT("equals"), TEXT("ne"), TEXT("gt"), TEXT("gte"), TEXT("lt"), TEXT("lte"),
			TEXT("between"), TEXT("in"), TEXT("contains"), TEXT("matches"),
		};
		return List;
	}

	bool HasAny(const FJsonObject& InJson)
	{
		for (const FString& Key : Keys())
		{
			if (InJson.HasField(Key))
			{
				return true;
			}
		}
		return false;
	}

	namespace
	{
		bool IsRefText(const TSharedPtr<FJsonValue>& InValue)
		{
			FString Text;
			return InValue.IsValid() && InValue->TryGetString(Text) && (Text.StartsWith(TEXT("$")) || Text.StartsWith(TEXT("=")));
		}

		bool IsNumberish(const TSharedPtr<FJsonValue>& InValue)
		{
			if (!InValue.IsValid())
			{
				return false;
			}
			if (InValue->Type == EJson::Number || IsRefText(InValue))
			{
				return true;
			}
			FString Text;
			return InValue->TryGetString(Text) && Text.TrimStartAndEnd().IsNumeric();
		}

		const TCHAR* OpText(const FString& InKey)
		{
			if (InKey == TEXT("ne")) return TEXT("不等于");
			if (InKey == TEXT("gt")) return TEXT(">");
			if (InKey == TEXT("gte")) return TEXT(">=");
			if (InKey == TEXT("lt")) return TEXT("<");
			if (InKey == TEXT("lte")) return TEXT("<=");
			if (InKey == TEXT("between")) return TEXT("在区间");
			if (InKey == TEXT("in")) return TEXT("是其中之一");
			if (InKey == TEXT("contains")) return TEXT("包含");
			if (InKey == TEXT("matches")) return TEXT("匹配");
			return TEXT("等于");
		}

		FString JsonShort(const TSharedPtr<FJsonValue>& InValue)
		{
			return FGameTestValue::FromJson(InValue).ToDisplay(60);
		}

		bool Contains(const FGameTestValue& InHay, const FGameTestValue& InNeedle)
		{
			if (InHay.Type == EGameTestValueType::Array)
			{
				for (const FGameTestValue& Item : InHay.GetArray())
				{
					if (Item.LooseEquals(InNeedle))
					{
						return true;
					}
				}
				return false;
			}
			if (InHay.Type == EGameTestValueType::Map)
			{
				FGameTestValue Ignored;
				return InHay.TryGetField(InNeedle.ToDisplay(1000), Ignored);
			}
			return InHay.ToDisplay(100000).Contains(InNeedle.IsString() ? InNeedle.GetString() : InNeedle.ToDisplay(1000), ESearchCase::IgnoreCase);
		}
	}

	void Validate(const FJsonObject& InJson, FGameTestDiagSink& InSink)
	{
		for (const FString& Key : { FString(TEXT("gt")), FString(TEXT("gte")), FString(TEXT("lt")), FString(TEXT("lte")) })
		{
			const TSharedPtr<FJsonValue> Value = InJson.TryGetField(Key);
			if (Value.IsValid() && !IsNumberish(Value))
			{
				InSink.Error(TEXT("bad_type"), FString::Printf(TEXT("%s 要写数字（或 $变量、=表达式）"), *Key));
			}
		}
		if (const TSharedPtr<FJsonValue> Between = InJson.TryGetField(TEXT("between")))
		{
			const TArray<TSharedPtr<FJsonValue>>* Range = nullptr;
			if (!Between->TryGetArray(Range) || !Range || Range->Num() != 2 || !IsNumberish((*Range)[0]) || !IsNumberish((*Range)[1]))
			{
				InSink.Error(TEXT("bad_type"), TEXT("between 要写成两个数 [最小, 最大]"));
			}
		}
		if (const TSharedPtr<FJsonValue> In = InJson.TryGetField(TEXT("in")))
		{
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!IsRefText(In) && (!In->TryGetArray(Items) || !Items))
			{
				InSink.Error(TEXT("bad_type"), TEXT("in 要写成数组 [a, b, …]"));
			}
		}
		FString Pattern;
		if (InJson.TryGetStringField(TEXT("matches"), Pattern) && Pattern.IsEmpty())
		{
			InSink.Error(TEXT("bad_type"), TEXT("matches 的正则不能是空的"));
		}
	}

	bool Evaluate(const FJsonObject& InJson, const FGameTestValue& InActual, TFunctionRef<FGameTestValue(const TSharedPtr<FJsonValue>&)> InResolve, FString& OutSay)
	{
		if (InActual.IsUnresolved())
		{
			OutSay = FString::Printf(TEXT("读不到：%s"), *InActual.GetString());
			return false;
		}
		bool bAny = false;
		for (const FString& Key : Keys())
		{
			const TSharedPtr<FJsonValue> Raw = InJson.TryGetField(Key);
			if (!Raw.IsValid())
			{
				continue;
			}
			bAny = true;
			bool bHolds = false;
			FString ExpectShown;
			if (Key == TEXT("between"))
			{
				const TArray<TSharedPtr<FJsonValue>>* Range = nullptr;
				if (Raw->TryGetArray(Range) && Range && Range->Num() == 2)
				{
					const FGameTestValue Low = InResolve((*Range)[0]);
					const FGameTestValue High = InResolve((*Range)[1]);
					int32 A = 0;
					int32 B = 0;
					bHolds = InActual.Compare(Low, A) && InActual.Compare(High, B) && A >= 0 && B <= 0;
					ExpectShown = FString::Printf(TEXT("[%s, %s]"), *Low.ToDisplay(40), *High.ToDisplay(40));
				}
			}
			else if (Key == TEXT("in"))
			{
				// `in` 写数组时逐项解析（数组里可以有 $var），写 $var 时整体当数组。
				const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
				FGameTestValue List;
				if (Raw->TryGetArray(Items) && Items)
				{
					TArray<FGameTestValue> Values;
					for (const TSharedPtr<FJsonValue>& Item : *Items)
					{
						Values.Add(InResolve(Item));
					}
					List = FGameTestValue::FromArray(MoveTemp(Values));
				}
				else
				{
					List = InResolve(Raw);
				}
				bHolds = List.Type == EGameTestValueType::Array && Contains(List, InActual);
				ExpectShown = List.ToDisplay(80);
			}
			else
			{
				const FGameTestValue Expect = InResolve(Raw);
				ExpectShown = Expect.ToDisplay(60);
				int32 Order = 0;
				if (Expect.IsUnresolved())
				{
					OutSay = FString::Printf(TEXT("期望值读不到：%s"), *Expect.GetString());
					return false;
				}
				if (Key == TEXT("equals"))
				{
					bHolds = InActual.LooseEquals(Expect);
				}
				else if (Key == TEXT("ne"))
				{
					bHolds = !InActual.LooseEquals(Expect);
				}
				else if (Key == TEXT("gt"))
				{
					bHolds = InActual.Compare(Expect, Order) && Order > 0;
				}
				else if (Key == TEXT("gte"))
				{
					bHolds = InActual.Compare(Expect, Order) && Order >= 0;
				}
				else if (Key == TEXT("lt"))
				{
					bHolds = InActual.Compare(Expect, Order) && Order < 0;
				}
				else if (Key == TEXT("lte"))
				{
					bHolds = InActual.Compare(Expect, Order) && Order <= 0;
				}
				else if (Key == TEXT("contains"))
				{
					bHolds = Contains(InActual, Expect);
				}
				else if (Key == TEXT("matches"))
				{
					const FRegexPattern Pattern(Expect.IsString() ? Expect.GetString() : Expect.ToDisplay(1000));
					FRegexMatcher Matcher(Pattern, InActual.IsString() ? InActual.GetString() : InActual.ToDisplay(100000));
					bHolds = Matcher.FindNext();
				}
			}
			if (!bHolds)
			{
				OutSay = FString::Printf(TEXT("期望%s %s，实际 %s"), OpText(Key), *ExpectShown, *InActual.ToDisplay(80));
				return false;
			}
		}
		if (!bAny && !InActual.Truthy())
		{
			OutSay = FString::Printf(TEXT("期望为真，实际 %s"), *InActual.ToDisplay(80));
			return false;
		}
		return true;
	}

	void EvaluateInto(const FJsonObject& InJson, const FGameTestValue& InActual, FGameTestFlowContext& InContext, FGameTestCheckResult& OutResult)
	{
		OutResult.Actual = InActual;
		if (InActual.IsUnresolved())
		{
			OutResult.bHolds = false;
			OutResult.Unresolved = InActual.GetString();
			return;
		}
		OutResult.bHolds = Evaluate(InJson, InActual,
			[&InContext](const TSharedPtr<FJsonValue>& InExpected) { return GameTestRefs::EvalJson(InExpected, InContext); }, OutResult.Say);
	}

	FString Describe(const FJsonObject& InJson)
	{
		TArray<FString> Parts;
		for (const FString& Key : Keys())
		{
			const TSharedPtr<FJsonValue> Raw = InJson.TryGetField(Key);
			if (Raw.IsValid())
			{
				Parts.Add(FString::Printf(TEXT("%s %s"), OpText(Key), *JsonShort(Raw)));
			}
		}
		return Parts.Num() > 0 ? FString::Join(Parts, TEXT("，")) : TEXT("为真");
	}
}
