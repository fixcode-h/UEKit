// Copyright Epic Games, Inc. All Rights Reserved.

#include "Api/GameTestApi.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTestCompat.h"
#include "Observe/Checks/GameTestCheck.h"

/** 判断：只读接口 `{"api": "Widget.IsShown", "args": {"target": "WBP_Shop"}}` + 比较（没写比较按真值）。 */
namespace
{
	/** `args` 写对象按名字给，写数组按顺序给，求值后排成接口参数的顺序。 */
	bool BuildArgs(const FGameTestApiDesc& InDesc, const FJsonObject& InJson, FGameTestFlowContext& InContext, TArray<FGameTestValue>& OutArgs, FString& OutError)
	{
		FName Code;
		const TSharedPtr<FJsonValue> Raw = InJson.TryGetField(TEXT("args"));
		if (!Raw.IsValid())
		{
			const TArray<FGameTestValue> None;
			return GameTestApis::BindArgs(InDesc, nullptr, &None, OutArgs, Code, OutError);
		}
		const FGameTestValue Args = GameTestRefs::EvalJson(Raw, InContext);
		if (Args.IsUnresolved())
		{
			OutError = Args.GetString();
			return false;
		}
		if (Args.Type == EGameTestValueType::Array)
		{
			return GameTestApis::BindArgs(InDesc, nullptr, &Args.GetArray(), OutArgs, Code, OutError);
		}
		return GameTestApis::BindArgs(InDesc, &Args.GetMap(), nullptr, OutArgs, Code, OutError);
	}

	class FCheckApi : public IGameTestCheck
	{
	public:
		virtual FName GetKey() const override { return TEXT("api"); }
		virtual FString GetSummary() const override { return TEXT("只读接口的返回值"); }
		virtual bool Matches(const FJsonObject& InJson) const override { return InJson.HasField(TEXT("api")); }

		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields = { TEXT("api"), TEXT("args") };
			return Fields;
		}

		virtual void Validate(const FJsonObject& InJson, FGameTestDiagSink& InSink) const override
		{
			FString Name;
			if (!InJson.TryGetStringField(TEXT("api"), Name))
			{
				InSink.Error(TEXT("bad_type"), TEXT("api 要写接口名（字符串）"));
				return;
			}
			const IGameTestApi* Api = GameTestApis::Get().Find(FName(*Name));
			if (!Api)
			{
				const FString* NewName = GameTestApis::GetRenamed().Find(Name);
				InSink.Error(NewName ? TEXT("api_renamed") : TEXT("api_missing"),
					NewName ? FString::Printf(TEXT("接口 %s 已改名，改用 %s"), *Name, **NewName) : FString::Printf(TEXT("没有接口 %s"), *Name));
				return;
			}
			if (!Api->GetDesc().bReadOnly || Api->GetDesc().bMayWait)
			{
				InSink.Error(TEXT("expr_not_readonly"), FString::Printf(TEXT("%s 会改游戏或要等待，不能当条件，改成 api 步骤"), *Name));
			}
		}

		virtual void Evaluate(const FJsonObject& InJson, FGameTestFlowContext& InContext, FGameTestCheckResult& OutResult) const override
		{
			FString Name;
			InJson.TryGetStringField(TEXT("api"), Name);
			const IGameTestApi* Api = GameTestApis::Get().Find(FName(*Name));
			FGameTestValue Actual;
			TArray<FGameTestValue> Args;
			FString Error;
			if (!Api)
			{
				Actual = GameTestApis::CallNow(Name, Args, InContext);
			}
			else if (!BuildArgs(Api->GetDesc(), InJson, InContext, Args, Error))
			{
				Actual = FGameTestValue::Unresolved(Error);
			}
			else
			{
				// 按位置调：缺的可选参数已经是 Null，接口按「没给」处理。
				while (Args.Num() > 0 && Args.Last().IsNull())
				{
					Args.Pop();
				}
				Actual = GameTestApis::CallNow(Name, Args, InContext);
			}
			GameTestCompare::EvaluateInto(InJson, Actual, InContext, OutResult);
		}

		virtual FString Describe(const FJsonObject& InJson) const override
		{
			FString Name;
			InJson.TryGetStringField(TEXT("api"), Name);
			return FString::Printf(TEXT("%s(…) %s"), *Name, *GameTestCompare::Describe(InJson));
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterCheckApi()
	{
		GameTestChecks::Get().Register(MakeShared<FCheckApi>());
	}
}
