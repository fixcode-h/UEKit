// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Expr/GameTestExpr.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Checks/GameTestCheck.h"
#include "Observe/Checks/GameTestCond.h"

/**
 * 判断：代码变量和脚本变量 `{"value": "@ps.Gold", "gte": 500}`、`{"value": "$count", "equals": 3}`。
 * 没写 `value` 只写比较时比的是 `invoke expect` / `api expect` 的返回值。最后登记：别的判断都不认时才轮到它。
 */
namespace
{
	/** `value` 里 `@` 开头的是来源路径（不用再加 `=`），其余按填值规则（`$var`、`=表达式`、字面量）。 */
	FGameTestValue ReadValue(const TSharedPtr<FJsonValue>& InRaw, FGameTestFlowContext& InContext)
	{
		FString Text;
		if (InRaw.IsValid() && InRaw->TryGetString(Text) && Text.StartsWith(TEXT("@")))
		{
			return GameTestExprCache::Eval(Text, InContext);
		}
		return GameTestRefs::EvalJson(InRaw, InContext);
	}

	class FCheckValue : public IGameTestCheck
	{
	public:
		virtual FName GetKey() const override { return TEXT("value"); }
		virtual FString GetSummary() const override { return TEXT("代码变量、脚本变量或返回值和期望比较"); }

		virtual bool Matches(const FJsonObject& InJson) const override
		{
			return InJson.HasField(TEXT("value")) || GameTestCompare::HasAny(InJson);
		}

		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields = { TEXT("value") };
			return Fields;
		}

		virtual void Validate(const FJsonObject& InJson, FGameTestDiagSink& InSink) const override
		{
			FString Text;
			if (InJson.TryGetStringField(TEXT("value"), Text) && Text.StartsWith(TEXT("@")))
			{
				FString Error;
				if (!GameTestExprCache::Get(Text, Error).IsValid())
				{
					InSink.Error(TEXT("expr_syntax"), Error);
				}
			}
		}

		virtual void Evaluate(const FJsonObject& InJson, FGameTestFlowContext& InContext, FGameTestCheckResult& OutResult) const override
		{
			FGameTestValue Actual;
			if (const TSharedPtr<FJsonValue> Raw = InJson.TryGetField(TEXT("value")))
			{
				Actual = ReadValue(Raw, InContext);
			}
			else if (!InContext.FindVar(GameTestCond::SubjectVar(), Actual))
			{
				Actual = FGameTestValue::Unresolved(TEXT("没写 value，这里也没有返回值可比"));
			}
			GameTestCompare::EvaluateInto(InJson, Actual, InContext, OutResult);
		}

		virtual FString Describe(const FJsonObject& InJson) const override
		{
			FString Text;
			if (!InJson.TryGetStringField(TEXT("value"), Text))
			{
				Text = InJson.HasField(TEXT("value")) ? TEXT("值") : TEXT("返回值");
			}
			return FString::Printf(TEXT("%s %s"), *Text, *GameTestCompare::Describe(InJson));
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterCheckValue()
	{
		GameTestChecks::Get().Register(MakeShared<FCheckValue>());
	}
}
