// Copyright Epic Games, Inc. All Rights Reserved.

// obj.call 动作（docs/game-test-ref.md 5.3，取代 1.x 的 invoke）：在对象上调函数（反射或 Lua），返回值存 saveAs、用 expect 判断。
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
#include "Operate/Obj/GameTestObjActions.h"
#include "Components/Widget.h"

namespace GameTestInvokeLocal
{
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

	/** Lua：`@luaMod.模块`（模块上的函数）或绑在对象上的实例表（反射里没有这个函数的对象）。 */
	FGameTestValue CallLua(FGameTestFlowContext& InFlow, UObject* InSelf, const FString& InModule, const FString& InFunction, const FGameTestValue& InArgs, TSharedPtr<FJsonObject>& OutPayload)
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

	FGameTestValue CallOnObject(FGameTestFlowContext& InFlow, UObject* InTarget, const FString& InFunction, const FGameTestValue& InArgs, TSharedPtr<FJsonObject>& OutPayload)
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

	/** 调完：saveAs、expect，消息写进 InContext。 */
	EGameTestStepStatus Finish(FGameTestStepContext& InContext, const FString& InFunction, const FGameTestValue& InValue)
	{
		FGameTestFlowContext& Flow = *InContext.Flow;
		if (InValue.IsUnresolved())
		{
			InContext.Message = FString::Printf(TEXT("调 %s 失败：%s"), *InFunction, *InValue.GetString());
			InContext.Payload->SetStringField(TEXT("code"), InValue.GetCode().ToString());
			return GameTestHandlers::StepFailed;
		}
		FString SaveAs;
		if (InContext.Step->Params->TryGetStringField(TEXT("saveAs"), SaveAs) && !SaveAs.IsEmpty())
		{
			Flow.SetVar(SaveAs, InValue);
		}
		const TSharedPtr<FJsonObject> Expect = GameTestCond::AsObject(InContext.Step->Params->TryGetField(TEXT("expect")));
		if (Expect.IsValid())
		{
			// expect 是统一条件，主语是返回值（判断里不写 obj / value 时看它）
			FGameTestCondResult Result;
			{
				FGameTestFlowContext::FReadOnlyScope ReadOnly(Flow);
				Result = GameTestCond::Evaluate(*Expect, Flow, &InValue);
			}
			InContext.Payload->SetObjectField(TEXT("cond"), Result.ToJson());
			if (!Result.Holds())
			{
				InContext.Message = FString::Printf(TEXT("%s 返回 %s，不满足 expect：%s"), *InFunction, *InValue.ToDisplay(200), *Result.Summary());
				return GameTestHandlers::StepFailed;
			}
		}
		InContext.Message = FString::Printf(TEXT("已调 %s → %s"), *InFunction, *InValue.ToDisplay(200));
		return GameTestHandlers::StepSucceeded;
	}

	/** 调用期间把反射调用写的 payload.invoke 指到这一步上。 */
	struct FPayloadScope
	{
		FGameTestFlowContext& Flow;
		TSharedPtr<FJsonObject> Prev;
		FPayloadScope(FGameTestFlowContext& InFlow, const TSharedPtr<FJsonObject>& InPayload)
			: Flow(InFlow), Prev(InFlow.StepPayload)
		{
			Flow.StepPayload = InPayload;
		}
		~FPayloadScope()
		{
			Flow.StepPayload = Prev;
		}
	};

	/** 不用等对象的几种：静态函数（省略 obj）、`@luaMod.模块`、`@来源`（子系统、玩家…）。 */
	FGameTestValue CallDirect(FGameTestStepContext& InContext, FGameTestFlowContext& InFlow, const TSharedPtr<FJsonValue>& InObj, const FString& InFunction, const FGameTestValue& InArgs)
	{
		if (!InObj.IsValid() || InObj->IsNull())
		{
			// 没写对象：类名.函数 的静态函数（蓝图函数库、static UFUNCTION）
			UObject* Target = nullptr;
			FString Error;
			UFunction* Static = GameTestReflectCall::FindStatic(InFunction, Target, Error);
			if (Static == nullptr)
			{
				return FGameTestValue::Unresolved(Error, TEXT("fn_missing"));
			}
			GameTestReflectCall::FArgs Named;
			Named.Named = InArgs.Type == EGameTestValueType::Map ? &InArgs.GetMap() : nullptr;
			Named.Positional = InArgs.Type == EGameTestValueType::Array ? &InArgs.GetArray() : nullptr;
			return GameTestReflectCall::Call(Target, Static, Named, InFlow);
		}
		FString ObjText;
		InObj->TryGetString(ObjText);
		if (ObjText.StartsWith(TEXT("@luaMod.")))
		{
			return CallLua(InFlow, nullptr, ObjText.Mid(8), InFunction, InArgs, InContext.Payload);
		}
		const FGameTestValue ObjectValue = GameTestRefs::EvalJson(InObj, InFlow);
		FString Error;
		UObject* Target = GameTestApiArgs::Object(ObjectValue, InFlow, Error);
		if (Target == nullptr)
		{
			return FGameTestValue::Unresolved(Error, ObjectValue.IsUnresolved() ? ObjectValue.GetCode() : FName(TEXT("arg_object")));
		}
		return CallOnObject(InFlow, Target, InFunction, InArgs, InContext.Payload);
	}
}

namespace GameTestHandlers
{
	EGameTestStepStatus HandleObjCall(FGameTestStepContext& InContext)
	{
		using namespace GameTestInvokeLocal;
		if (InContext.Flow == nullptr)
		{
			return GameTestObjActions::NoFlow(InContext, TEXT("obj.call"));
		}
		FGameTestFlowContext& Flow = *InContext.Flow;
		Flow.World = InContext.World ? InContext.World : Flow.World;
		const FJsonObject& Params = *InContext.Step->Params;
		FString Function;
		Params.TryGetStringField(TEXT("function"), Function);
		if (Function.IsEmpty())
		{
			InContext.Message = TEXT("obj.call 要写 function");
			return StepFailed;
		}
		const FGameTestValue Args = GameTestRefs::EvalJson(Params.TryGetField(TEXT("args")), Flow);
		if (Args.IsUnresolved())
		{
			return Finish(InContext, Function, Args);
		}
		const TSharedPtr<FJsonValue> Obj = Params.TryGetField(TEXT("obj"));
		FString ObjText;
		if (!Obj.IsValid() || Obj->IsNull() || (Obj->TryGetString(ObjText) && ObjText.StartsWith(TEXT("@"))))
		{
			FGameTestValue Value;
			{
				FPayloadScope Scope(Flow, InContext.Payload);
				Value = CallDirect(InContext, Flow, Obj, Function, Args);
			}
			return Finish(InContext, Function, Value);
		}
		// 对象引用（+ part）：等它出现再调
		return GameTestObjActions::RunWhenFound(InContext, FString::Printf(TEXT("调 %s"), *Function), [Function, Args](FGameTestStepContext& InStep, UObject* InObject)
		{
			FGameTestValue Value;
			{
				FPayloadScope Scope(*InStep.Flow, InStep.Payload);
				Value = CallOnObject(*InStep.Flow, InObject, Function, Args, InStep.Payload);
			}
			return Finish(InStep, Function, Value);
		});
	}
}
