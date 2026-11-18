// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Expr/GameTestExpr.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Observe/Checks/GameTestCheck.h"

/** 判断：`{"expr": "@ps.Gold >= $gold0 + 500"}`，按真值算（也可以再写比较）。 */
namespace
{
	class FCheckExpr : public IGameTestCheck
	{
	public:
		virtual FName GetKey() const override { return TEXT("expr"); }
		virtual FString GetSummary() const override { return TEXT("表达式成立（真值）"); }
		virtual bool Matches(const FJsonObject& InJson) const override { return InJson.HasField(TEXT("expr")); }

		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields = { TEXT("expr") };
			return Fields;
		}

		virtual void Validate(const FJsonObject& InJson, FGameTestDiagSink& InSink) const override
		{
			FString Text;
			if (!InJson.TryGetStringField(TEXT("expr"), Text))
			{
				InSink.Error(TEXT("bad_type"), TEXT("expr 要写成字符串"));
				return;
			}
			FString Error;
			if (!GameTestExprCache::Get(Text.StartsWith(TEXT("=")) ? Text.RightChop(1) : Text, Error).IsValid())
			{
				InSink.Error(TEXT("expr_syntax"), Error);
			}
		}

		virtual void Evaluate(const FJsonObject& InJson, FGameTestFlowContext& InContext, FGameTestCheckResult& OutResult) const override
		{
			FString Text;
			InJson.TryGetStringField(TEXT("expr"), Text);
			const FGameTestValue Value = GameTestExprCache::Eval(Text.StartsWith(TEXT("=")) ? Text.RightChop(1) : Text, InContext);
			GameTestCompare::EvaluateInto(InJson, Value, InContext, OutResult);
		}

		virtual FString Describe(const FJsonObject& InJson) const override
		{
			FString Text;
			InJson.TryGetStringField(TEXT("expr"), Text);
			return GameTestCompare::HasAny(InJson) ? FString::Printf(TEXT("%s %s"), *Text, *GameTestCompare::Describe(InJson)) : Text;
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterCheckExpr()
	{
		GameTestChecks::Get().Register(MakeShared<FCheckExpr>());
	}
}
