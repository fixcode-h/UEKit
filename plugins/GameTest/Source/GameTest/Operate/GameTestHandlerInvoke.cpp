// Copyright Epic Games, Inc. All Rights Reserved.

// invoke 动作（协议第 6 节）：在对象上调函数（反射或 Lua），返回值存 saveAs、用 expect 判断。
// 参数构造和调用复用 GameTestReflectCall（= UELink ue.call 同一份代码），Lua 走 UELink 的 lua.call。

#include "Api/Builtin/GameTestApiArgs.h"
#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestRunner.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Checks/GameTestCond.h"
#include "Observe/GameTestWidgetNode.h"
#include "Observe/Lua/GameTestLuaBridge.h"
#include "Observe/Sources/GameTestReflectCall.h"
#include "Observe/Sources/GameTestReflectValue.h"
#include "Observe/Sources/GameTestValueSource.h"
#include "Components/Widget.h"

namespace GameTestInvokeLocal
{
	/** 这次调用的结果：值，或失败原因和错误码。 */
	struct FOutcome
	{
		FGameTestValue Value;
		bool Failed() const { return Value.IsUnresolved(); }
	};

	TArray<TSharedPtr<FJsonValue>> PositionalJson(const FGameTestValue& InArgs)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		if (InArgs.Type == EGameTestValueType::Array)
		{
			for (const FGameTestValue& Item : InArgs.GetArray())
			{
				Out.Add(Item.ToJson());
			}
		}
		else if (InArgs.Type == EGameTestValueType::Map)
		{
			// Lua 函数没有参数名：按写的顺序传（JSON 对象的顺序就是场景里写的顺序）
			for (const TPair<FString, FGameTestValue>& Pair : InArgs.GetMap())
			{
				Out.Add(Pair.Value.ToJson());
			}
		}
		else if (!InArgs.IsNull())
		{
			Out.Add(InArgs.ToJson());
		}
		return Out;
	}

	FGameTestValue FromLuaResults(const TArray<FGameTestLuaValue>& InResults)
	{
		if (InResults.Num() <= 1)
		{
			return InResults.Num() == 1 ? InResults[0].ToValue() : FGameTestValue::Null();
		}
		TArray<FGameTestValue> Items;
		for (const FGameTestLuaValue& Result : InResults)
		{
			Items.Add(Result.ToValue());
		}
		return FGameTestValue::FromArray(MoveTemp(Items));
	}

	/** Lua：`@luaMod.模块`（模块上的函数）或绑在对象上的实例表（`@lua` 配 target、或反射里没有这个函数的对象）。 */
	FGameTestValue CallLua(FGameTestFlowContext& InFlow, UObject* InSelf, const FString& InModule, const FString& InFunction,
		const FGameTestValue& InArgs, TSharedPtr<FJsonObject>& OutPayload)
	{
		if (!InFlow.Options.bAllowLuaInvoke)
		{
			return FGameTestValue::Unresolved(TEXT("运行配置关掉了步骤调 Lua（_settings.json 的 allowLuaInvoke）"), TEXT("lua_disabled"));
		}
		const double Start = FPlatformTime::Seconds();
		TArray<FGameTestLuaValue> Results;
		FString Error;
		const TArray<TSharedPtr<FJsonValue>> Args = PositionalJson(InArgs);
		if (!GameTestLuaBridge::Call(InFlow.World, InSelf, InModule, GameTestLuaBridge::SplitPath(InFunction), Args, Results, Error))
		{
			return FGameTestValue::Unresolved(Error, TEXT("fn_missing"));
		}
		const FGameTestValue Value = FromLuaResults(Results);
		const TSharedRef<FJsonObject> Invoke = MakeShared<FJsonObject>();
		Invoke->SetStringField(TEXT("object"), InModule.IsEmpty() ? GameTestApiArgs::NameOf(InSelf) : TEXT("@luaMod.") + InModule);
		Invoke->SetStringField(TEXT("function"), InFunction);
		Invoke->SetArrayField(TEXT("args"), Args);
		Invoke->SetStringField(TEXT("returned"), Value.ToDisplay(500));
		Invoke->SetNumberField(TEXT("ms"), FMath::RoundToInt((FPlatformTime::Seconds() - Start) * 1000.0));
		OutPayload->SetObjectField(TEXT("invoke"), Invoke);
		return Value;
	}

	FGameTestValue CallOnObject(FGameTestFlowContext& InFlow, UObject* InTarget, const FString& InFunction, const FGameTestValue& InArgs,
		TSharedPtr<FJsonObject>& OutPayload)
	{
		UFunction* Function = GameTestReflectValue::FindFunction(InTarget->GetClass(), InFunction);
		if (Function == nullptr)
		{
			// 反射里没有：可能是绑的 Lua 实例表上的函数（UnLua 覆写的界面、Actor）
			return CallLua(InFlow, InTarget, FString(), InFunction, InArgs, OutPayload);
		}
		GameTestReflectCall::FArgs Args;
		if (InArgs.Type == EGameTestValueType::Map)
		{
			Args.Named = &InArgs.GetMap();
		}
		else if (InArgs.Type == EGameTestValueType::Array)
		{
			Args.Positional = &InArgs.GetArray();
		}
		return GameTestReflectCall::Call(InTarget, Function, Args, InFlow);
	}

	FOutcome Invoke(FGameTestStepContext& InContext, FGameTestFlowContext& InFlow)
	{
		const FJsonObject& Params = *InContext.Step->Params;
		FString Function;
		Params.TryGetStringField(TEXT("function"), Function);
		if (Function.IsEmpty())
		{
			return { FGameTestValue::Unresolved(TEXT("invoke 要写 function"), TEXT("arg_missing")) };
		}
		const FGameTestValue Args = GameTestRefs::EvalJson(Params.TryGetField(TEXT("args")), InFlow);
		if (Args.IsUnresolved())
		{
			return { Args };
		}
		const TSharedPtr<FJsonValue> ObjectJson = Params.TryGetField(TEXT("object"));
		if (!ObjectJson.IsValid() || ObjectJson->IsNull())
		{
			// 没写对象：类名.函数 的静态函数（蓝图函数库、static UFUNCTION）
			UObject* Target = nullptr;
			FString Error;
			UFunction* Static = GameTestReflectCall::FindStatic(Function, Target, Error);
			if (Static == nullptr)
			{
				return { FGameTestValue::Unresolved(Error, TEXT("fn_missing")) };
			}
			GameTestReflectCall::FArgs Named;
			Named.Named = Args.Type == EGameTestValueType::Map ? &Args.GetMap() : nullptr;
			Named.Positional = Args.Type == EGameTestValueType::Array ? &Args.GetArray() : nullptr;
			return { GameTestReflectCall::Call(Target, Static, Named, InFlow) };
		}
		FString ObjectText;
		if (ObjectJson->TryGetString(ObjectText))
		{
			if (ObjectText.StartsWith(TEXT("@luaMod.")))
			{
				return { CallLua(InFlow, nullptr, ObjectText.Mid(8), Function, Args, InContext.Payload) };
			}
			if (ObjectText == TEXT("@lua"))
			{
				// @lua 配 target：界面绑的 Lua 实例表
				TSharedPtr<FGameTestWidgetNode> Node;
				FString Error;
				if (!GameTestHandlers::TryResolveStepTarget(InContext, Node, Error))
				{
					return { FGameTestValue::Unresolved(Error, TEXT("selector_missing")) };
				}
				return { CallLua(InFlow, Node->Widget.Get(), FString(), Function, Args, InContext.Payload) };
			}
		}
		const FGameTestValue ObjectValue = GameTestRefs::EvalJson(ObjectJson, InFlow);
		FString Error;
		UObject* Target = GameTestApiArgs::Object(ObjectValue, InFlow, Error);
		if (Target == nullptr)
		{
			return { FGameTestValue::Unresolved(Error, ObjectValue.IsUnresolved() ? ObjectValue.GetCode() : FName(TEXT("arg_object"))) };
		}
		return { CallOnObject(InFlow, Target, Function, Args, InContext.Payload) };
	}
}

namespace GameTestHandlers
{
	EGameTestStepStatus HandleInvoke(FGameTestStepContext& InContext)
	{
		using namespace GameTestInvokeLocal;
		FGameTestFlowContext* Flow = InContext.Flow;
		if (Flow == nullptr)
		{
			InContext.Message = TEXT("这一步没有脚本上下文（invoke 只能在 1.9 的场景里用）");
			return StepFailed;
		}
		// 反射调用把 payload.invoke 写进 StepPayload：指到这一步的 payload 上
		const TSharedPtr<FJsonObject> PrevPayload = Flow->StepPayload;
		Flow->StepPayload = InContext.Payload;
		Flow->World = InContext.World ? InContext.World : Flow->World;
		const FOutcome Outcome = Invoke(InContext, *Flow);
		Flow->StepPayload = PrevPayload;

		const FString Function = InContext.Step->Params->GetStringField(TEXT("function"));
		if (Outcome.Failed())
		{
			InContext.Message = FString::Printf(TEXT("调 %s 失败：%s"), *Function, *Outcome.Value.GetString());
			InContext.Payload->SetStringField(TEXT("code"), Outcome.Value.GetCode().ToString());
			return StepFailed;
		}

		FString SaveAs;
		if (InContext.Step->Params->TryGetStringField(TEXT("saveAs"), SaveAs) && !SaveAs.IsEmpty())
		{
			Flow->SetVar(SaveAs, Outcome.Value);
		}
		const TSharedPtr<FJsonObject> Expect = GameTestCond::AsObject(InContext.Step->Params->TryGetField(TEXT("expect")));
		if (Expect.IsValid())
		{
			// expect 是统一条件，主语是返回值（判断里不写 target / value 时看它）
			const FGameTestCondResult Result = GameTestCond::Evaluate(*Expect, *Flow, &Outcome.Value);
			InContext.Payload->SetObjectField(TEXT("cond"), Result.ToJson());
			if (!Result.Holds())
			{
				InContext.Message = FString::Printf(TEXT("%s 返回 %s，不满足 expect：%s"), *Function, *Outcome.Value.ToDisplay(200), *Result.Summary());
				return StepFailed;
			}
		}
		InContext.Message = FString::Printf(TEXT("已调 %s → %s"), *Function, *Outcome.Value.ToDisplay(200));
		return StepSucceeded;
	}
}
