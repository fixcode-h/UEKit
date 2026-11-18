// Copyright Epic Games, Inc. All Rights Reserved.

#include "Components/Widget.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTestCompat.h"
#include "Internationalization/Regex.h"
#include "Observe/Checks/GameTestCheck.h"
#include "Observe/Checks/GameTestWidgetProbe.h"
#include "Observe/GameTestCondition.h"
#include "Observe/GameTestWidgetNode.h"

/**
 * 判断：控件状态 `{"target": "WBP_Bag:Root/Btn", "enabled": true, "textLike": "领取"}`。
 *
 * 1.6 起的控件条件（exists / visible / enabled / text… / field + equals）原样交给 GameTestCondition，行为不变；
 * 1.9 新增的 `number`（文字里的数 + 比较）、`textMatches`（正则）、`field` + 任意比较在这里算。
 * 反面判断（exists:false…）在目标所在的屏不在屏上时记「还不能判断」，和 skipIf 同一条规则。
 */
namespace
{
	/** 交给 GameTestCondition 之前要拿掉的字段（它不认这些）。 */
	const TArray<FString>& OwnKeys()
	{
		static const TArray<FString> Keys = {
			TEXT("target"), TEXT("number"), TEXT("textMatches"), TEXT("client"), TEXT("note"), TEXT("settleMs"), TEXT("elseSettleMs"), TEXT("hint"),
		};
		return Keys;
	}

	/** `field` 配了比较（equals 以外的，或者期望值是 $var / =表达式）时由这里算，否则仍交给旧逻辑。 */
	bool FieldHandledHere(const FJsonObject& InJson)
	{
		if (!InJson.HasField(TEXT("field")))
		{
			return false;
		}
		for (const FString& Key : GameTestCompare::Keys())
		{
			if (Key != TEXT("equals") && InJson.HasField(Key))
			{
				return true;
			}
		}
		FString Expected;
		return InJson.TryGetStringField(TEXT("equals"), Expected) && (Expected.StartsWith(TEXT("$")) || Expected.StartsWith(TEXT("=")));
	}

	TSharedPtr<FJsonObject> LegacyPart(const FJsonObject& InJson)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		const bool bFieldHere = FieldHandledHere(InJson);
		for (const auto& Pair : InJson.Values)
		{
			const FString Key = GAMETEST_JSON_KEY(Pair.Key);
			if (OwnKeys().Contains(Key) || (bFieldHere && (Key == TEXT("field") || GameTestCompare::Keys().Contains(Key))))
			{
				continue;
			}
			Out->SetField(Key, Pair.Value);
		}
		return Out;
	}

	/** `number` 写一个数就是等于，写对象就是比较（`{"gte": 500}`）。 */
	void CheckNumber(const FJsonObject& InJson, const FGameTestWidgetNode& InNode, FGameTestFlowContext& InContext, FGameTestCheckResult& OutResult)
	{
		const FString Text = GameTestCondition::ReadText(InNode);
		double Number = 0.0;
		const FGameTestValue Actual = GameTestWidgetProbe::NumberInText(Text, Number) ? FGameTestValue::FromNumber(Number)
			: FGameTestValue::Unresolved(FString::Printf(TEXT("文字「%s」里没有数字"), *Text.Left(40)));
		const TSharedPtr<FJsonObject>* Compare = nullptr;
		TSharedPtr<FJsonObject> Wrapped = MakeShared<FJsonObject>();
		if (!InJson.TryGetObjectField(TEXT("number"), Compare) || !Compare)
		{
			Wrapped->SetField(TEXT("equals"), InJson.TryGetField(TEXT("number")));
			Compare = &Wrapped;
		}
		GameTestCompare::EvaluateInto(**Compare, Actual, InContext, OutResult);
	}

	class FCheckWidget : public IGameTestCheck
	{
	public:
		virtual FName GetKey() const override { return TEXT("widget"); }
		virtual FString GetSummary() const override { return TEXT("控件状态：存在、可见、可点、勾选、文字、文字里的数、字段"); }
		virtual bool Matches(const FJsonObject& InJson) const override { return InJson.HasField(TEXT("target")); }

		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields = []()
			{
				TArray<FString> All = GameTestCondition::Keys();
				All.Append({ TEXT("target"), TEXT("number"), TEXT("textMatches"), TEXT("hint") });
				return All;
			}();
			return Fields;
		}

		virtual void Validate(const FJsonObject& InJson, FGameTestDiagSink& InSink) const override
		{
			FString Error;
			const TSharedPtr<FJsonObject> Legacy = LegacyPart(InJson);
			if (Legacy->Values.Num() > 0 && !GameTestCondition::Validate(*Legacy, TArray<FString>(), Error))
			{
				InSink.Error(TEXT("bad_type"), Error);
			}
			if (Legacy->Values.Num() == 0 && !InJson.HasField(TEXT("number")) && !InJson.HasField(TEXT("textMatches")) && !FieldHandledHere(InJson))
			{
				InSink.Error(TEXT("bad_type"), TEXT("target 后面要写看什么：exists / visible / enabled / checked / text / number / field …"));
			}
			FString Pattern;
			if (InJson.HasField(TEXT("textMatches")) && (!InJson.TryGetStringField(TEXT("textMatches"), Pattern) || Pattern.IsEmpty()))
			{
				InSink.Error(TEXT("bad_type"), TEXT("textMatches 要写正则（字符串）"));
			}
		}

		virtual void Evaluate(const FJsonObject& InJson, FGameTestFlowContext& InContext, FGameTestCheckResult& OutResult) const override
		{
			FString Raw;
			InJson.TryGetStringField(TEXT("target"), Raw);
			const FGameTestWidgetLook Look = GameTestWidgetProbe::Look(Raw, InContext);
			if (IsNegative(InJson))
			{
				const FString Screen = GameTestWidgetProbe::ScreenOf(Look.Target);
				OutResult.bScreenAbsent = Screen.IsEmpty() || !GameTestWidgetProbe::IsScreenShown(Screen, GameTestWidgetProbe::WorldOf(InContext));
			}
			TArray<FString> Says;
			bool bHolds = true;
			if (Look.bResolved && InJson.HasField(TEXT("number")))
			{
				FGameTestCheckResult Part;
				CheckNumber(InJson, *Look.Node, InContext, Part);
				bHolds &= Part.bHolds;
				OutResult.Actual = Part.Actual;
				Says.Add(Part.Unresolved.IsEmpty() ? Part.Say : Part.Unresolved);
			}
			FString Pattern;
			if (Look.bResolved && InJson.TryGetStringField(TEXT("textMatches"), Pattern))
			{
				const FString Text = GameTestCondition::ReadText(*Look.Node);
				FRegexMatcher Matcher(FRegexPattern(Pattern), Text);
				const bool bMatch = Matcher.FindNext();
				bHolds &= bMatch;
				OutResult.Actual = FGameTestValue::FromString(Text);
				Says.Add(bMatch ? FString::Printf(TEXT("文字「%s」匹配"), *Text.Left(40)) : FString::Printf(TEXT("文字「%s」不匹配 %s"), *Text.Left(40), *Pattern));
			}
			if (Look.bResolved && FieldHandledHere(InJson))
			{
				FString Field;
				FString Why;
				InJson.TryGetStringField(TEXT("field"), Field);
				const TSharedPtr<FJsonValue> Read = GameTestCondition::ReadField(Look.Node->Widget.Get(), Field, Why);
				FGameTestCheckResult Part;
				GameTestCompare::EvaluateInto(InJson, Read.IsValid() ? FGameTestValue::FromJson(Read) : FGameTestValue::Unresolved(Why), InContext, Part);
				bHolds &= Part.bHolds;
				OutResult.Actual = Part.Actual;
				Says.Add(Part.Unresolved.IsEmpty() ? Part.Say : Part.Unresolved);
			}
			const TSharedPtr<FJsonObject> Legacy = LegacyPart(InJson);
			if (Legacy->Values.Num() > 0)
			{
				FGameTestConditionCheck Check;
				GameTestCondition::Check(*Legacy, Look.bResolved ? Look.Node.Get() : nullptr, Look.Error, Check);
				bHolds &= Check.Holds();
				Says.Append(Check.Holds() ? Check.Actuals : Check.Mismatches);
				if (OutResult.Actual.IsNull() && Check.Actuals.Num() > 0)
				{
					OutResult.Actual = FGameTestValue::FromString(FString::Join(Check.Actuals, TEXT("、")));
				}
			}
			else if (!Look.bResolved)
			{
				// 只写了 number / textMatches / field：控件不在就读不到。
				bHolds = false;
				OutResult.Unresolved = Look.Error;
			}
			OutResult.bHolds = bHolds;
			Says.RemoveAll([](const FString& Say) { return Say.IsEmpty(); });
			OutResult.Say = FString::Join(Says, TEXT("；"));
		}

		virtual bool IsNegative(const FJsonObject& InJson) const override
		{
			return GameTestCondition::HasNegative(*LegacyPart(InJson));
		}

		virtual FString Describe(const FJsonObject& InJson) const override
		{
			FString Target;
			InJson.TryGetStringField(TEXT("target"), Target);
			TArray<FString> Parts;
			const TSharedPtr<FJsonObject> Legacy = LegacyPart(InJson);
			if (Legacy->Values.Num() > 0)
			{
				Parts.Add(GameTestCondition::Describe(*Legacy));
			}
			if (InJson.HasField(TEXT("number")))
			{
				const TSharedPtr<FJsonObject>* Compare = nullptr;
				Parts.Add(InJson.TryGetObjectField(TEXT("number"), Compare) && Compare
					? FString::Printf(TEXT("文字里的数 %s"), *GameTestCompare::Describe(**Compare))
					: FString::Printf(TEXT("文字里的数等于 %s"), *FGameTestValue::FromJson(InJson.TryGetField(TEXT("number"))).ToDisplay(40)));
			}
			FString Pattern;
			if (InJson.TryGetStringField(TEXT("textMatches"), Pattern))
			{
				Parts.Add(FString::Printf(TEXT("文字匹配 %s"), *Pattern));
			}
			if (FieldHandledHere(InJson))
			{
				FString Field;
				InJson.TryGetStringField(TEXT("field"), Field);
				Parts.Add(FString::Printf(TEXT("%s %s"), *Field, *GameTestCompare::Describe(InJson)));
			}
			return FString::Printf(TEXT("%s：%s"), *Target, *FString::Join(Parts, TEXT("，")));
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterCheckWidget()
	{
		GameTestChecks::Get().Register(MakeShared<FCheckWidget>());
	}
}
