// Copyright Epic Games, Inc. All Rights Reserved.

// 1.9 脚本化的叶子动作（协议第 2、8 节）：api（调接口库），以及 assert / wait 认统一条件（cond、expr）。
// 都只是接口库和统一条件的薄包装：调函数、判断的实现各只有一份（Api/Builtin、Observe/Checks），这里不另写。
// 2.0.0 起 widget.close / widget.call 删掉了，关界面走 obj.close（Operate/Close）。

#include "Api/GameTestApi.h"
#include "Core/GameTestHandlerShared.h"
#include "GameTestCompat.h"
#include "Core/GameTestRunner.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Checks/GameTestCond.h"
#include "Observe/GameTestWidgetNode.h"

namespace GameTestScriptLeafLocal
{
	/** 没有脚本上下文时的统一报错（gt.step 单步也会给上下文，这里只防万一）。 */
	EGameTestStepStatus NoFlow(FGameTestStepContext& InContext, const TCHAR* InAction)
	{
		InContext.Message = FString::Printf(TEXT("这一步没有脚本上下文（%s 只能在 1.9 的场景里用）"), InAction);
		return GameTestHandlers::StepFailed;
	}

	/** api 步骤的参数：cond 类型的参数原样交给接口（接口自己判断），其余求值（`$var`、`=表达式`、`@来源`）。 */
	bool BindStepArgs(const FGameTestApiDesc& InDesc, const FJsonObject& InParams, FGameTestFlowContext& InFlow, TArray<FGameTestValue>& OutArgs, FName& OutCode, FString& OutError)
	{
		TMap<FString, FGameTestValue> Named;
		const TSharedPtr<FJsonValue> ArgsJson = InParams.TryGetField(TEXT("args"));
		const TSharedPtr<FJsonObject>* ArgsObject = nullptr;
		if (ArgsJson.IsValid() && !ArgsJson->IsNull())
		{
			if (!ArgsJson->TryGetObject(ArgsObject) || !ArgsObject || !ArgsObject->IsValid())
			{
				OutCode = TEXT("bad_type");
				OutError = FString::Printf(TEXT("api 的 args 要写成对象：{\"参数名\": 值}（%s）"), *InDesc.Signature());
				return false;
			}
			for (const auto& Pair : (*ArgsObject)->Values)
			{
				const FString Name = GAMETEST_JSON_KEY(Pair.Key);
				const FGameTestApiArg* Arg = InDesc.Args.FindByPredicate([&Name](const FGameTestApiArg& InArg) { return InArg.Name == Name; });
				const FGameTestValue Value = Arg != nullptr && Arg->Type == TEXT("cond") ? FGameTestValue::FromJson(Pair.Value)
					: GameTestRefs::EvalJson(Pair.Value, InFlow);
				if (Value.IsUnresolved())
				{
					OutCode = Value.GetCode();
					OutError = FString::Printf(TEXT("参数 %s 取不到值：%s"), *Name, *Value.GetString());
					return false;
				}
				Named.Add(Name, Value);
			}
		}
		return GameTestApis::BindArgs(InDesc, &Named, nullptr, OutArgs, OutCode, OutError);
	}

	/** 接口调完：saveAs、expect。 */
	EGameTestStepStatus FinishApi(FGameTestStepContext& InContext, const FString& InName, const FGameTestValue& InResult)
	{
		FGameTestFlowContext& Flow = *InContext.Flow;
		const FJsonObject& Params = *InContext.Step->Params;
		InContext.Payload->SetStringField(TEXT("returned"), InResult.ToDisplay(500));
		FString SaveAs;
		if (Params.TryGetStringField(TEXT("saveAs"), SaveAs) && !SaveAs.IsEmpty())
		{
			Flow.SetVar(SaveAs, InResult);
		}
		const TSharedPtr<FJsonObject> Expect = GameTestCond::AsObject(Params.TryGetField(TEXT("expect")));
		if (Expect.IsValid())
		{
			FGameTestCondResult Result;
			{
				FGameTestFlowContext::FReadOnlyScope ReadOnly(Flow);
				Result = GameTestCond::Evaluate(*Expect, Flow, &InResult);
			}
			InContext.Payload->SetObjectField(TEXT("cond"), Result.ToJson());
			if (!Result.Holds())
			{
				InContext.Message = FString::Printf(TEXT("%s 返回 %s，不满足 expect：%s"), *InName, *InResult.ToDisplay(200), *Result.Summary());
				return GameTestHandlers::StepFailed;
			}
		}
		InContext.Message = FString::Printf(TEXT("已调 %s → %s"), *InName, *InResult.ToDisplay(200));
		return GameTestHandlers::StepSucceeded;
	}

	/** 跨帧接口的半路状态：谓词和收尾都要拿到同一份调用。 */
	struct FPendingApi
	{
		FGameTestApiCall Call;
		bool bFinished = false;
	};
}

namespace GameTestHandlers
{
	EGameTestStepStatus HandleApi(FGameTestStepContext& InContext)
	{
		using namespace GameTestScriptLeafLocal;
		if (InContext.Flow == nullptr)
		{
			return NoFlow(InContext, TEXT("api"));
		}
		FGameTestFlowContext& Flow = *InContext.Flow;
		Flow.World = InContext.World ? InContext.World : Flow.World;
		const FJsonObject& Params = *InContext.Step->Params;
		FString Name;
		Params.TryGetStringField(TEXT("api"), Name);
		if (Name.IsEmpty())
		{
			InContext.Message = TEXT("api 步骤要写 api（接口名，如 Widget.IsShown；gt.api 列出全部）");
			return StepFailed;
		}
		const IGameTestApi* Api = GameTestApis::Get().Find(FName(*Name));
		if (Api == nullptr)
		{
			const FString* NewName = GameTestApis::GetRenamed().Find(Name);
			InContext.Message = NewName ? FString::Printf(TEXT("接口 %s 已改名，改用 %s"), *Name, **NewName)
				: FString::Printf(TEXT("没有接口 %s（gt.api 查可用的；项目接口改了先 gt.api.scan）"), *Name);
			InContext.Payload->SetStringField(TEXT("code"), NewName ? TEXT("api_renamed") : TEXT("api_missing"));
			return StepFailed;
		}

		const TSharedRef<FPendingApi> Pending = MakeShared<FPendingApi>();
		FName Code;
		FString Error;
		if (!BindStepArgs(Api->GetDesc(), Params, Flow, Pending->Call.Args, Code, Error))
		{
			InContext.Message = FString::Printf(TEXT("%s：%s"), *Name, *Error);
			InContext.Payload->SetStringField(TEXT("code"), Code.ToString());
			return StepFailed;
		}

		// 接口里写的 payload（如 invoke 记录）落到这一步上
		const TSharedPtr<FJsonObject> PrevPayload = Flow.StepPayload;
		Flow.StepPayload = InContext.Payload;
		const EGameTestCallState State = Api->Call(Pending->Call, Flow);
		Flow.StepPayload = PrevPayload;

		if (State == EGameTestCallState::Failed)
		{
			InContext.Message = Pending->Call.Error.IsEmpty() ? FString::Printf(TEXT("%s 调用失败"), *Name) : Pending->Call.Error;
			InContext.Payload->SetStringField(TEXT("code"), Pending->Call.ErrorCode.IsNone() ? TEXT("unresolved") : *Pending->Call.ErrorCode.ToString());
			return StepFailed;
		}
		if (State == EGameTestCallState::Done)
		{
			return FinishApi(InContext, Name, Pending->Call.Result);
		}
		if (!Pending->Call.Poll)
		{
			InContext.Message = FString::Printf(TEXT("%s 返回了「还在等」却没给 Poll（接口实现有问题）"), *Name);
			return StepFailed;
		}

		// 跨帧：每帧 Poll，结束时按结果收尾。谓词里拿不到 InContext（这一帧过后就没了），
		// 所以 saveAs / expect 的结论写进 payload 和失败消息，由执行器统一记录。
		InContext.PendingFailure = MakeShared<FString>();
		InContext.PendingProgress = MakeShared<FString>();
		const TSharedPtr<FString> Failure = InContext.PendingFailure;
		const TSharedPtr<FString> Progress = InContext.PendingProgress;
		const TSharedPtr<FJsonObject> Payload = InContext.Payload;
		const TSharedPtr<FJsonObject> Expect = GameTestCond::AsObject(Params.TryGetField(TEXT("expect")));
		FString SaveAs;
		Params.TryGetStringField(TEXT("saveAs"), SaveAs);
		FGameTestFlowContext* FlowPtr = &Flow;
		InContext.PendingPredicate = [Pending, FlowPtr, Name, Failure, Progress, Payload, Expect, SaveAs](double InNow)
		{
			const TSharedPtr<FJsonObject> Prev = FlowPtr->StepPayload;
			FlowPtr->StepPayload = Payload;
			const EGameTestCallState Now = Pending->Call.Poll(InNow);
			FlowPtr->StepPayload = Prev;
			if (Now == EGameTestCallState::Pending)
			{
				*Progress = FString::Printf(TEXT("%s 还在等"), *Name);
				return false;
			}
			Pending->bFinished = true;
			if (Now == EGameTestCallState::Failed)
			{
				*Failure = Pending->Call.Error.IsEmpty() ? FString::Printf(TEXT("%s 调用失败"), *Name) : Pending->Call.Error;
				Payload->SetStringField(TEXT("code"), Pending->Call.ErrorCode.IsNone() ? TEXT("unresolved") : *Pending->Call.ErrorCode.ToString());
				return true;
			}
			const FGameTestValue& Result = Pending->Call.Result;
			Payload->SetStringField(TEXT("returned"), Result.ToDisplay(500));
			if (!SaveAs.IsEmpty())
			{
				FlowPtr->SetVar(SaveAs, Result);
			}
			if (Expect.IsValid())
			{
				FGameTestCondResult Check;
				{
					FGameTestFlowContext::FReadOnlyScope ReadOnly(*FlowPtr);
					Check = GameTestCond::Evaluate(*Expect, *FlowPtr, &Result);
				}
				Payload->SetObjectField(TEXT("cond"), Check.ToJson());
				if (!Check.Holds())
				{
					*Failure = FString::Printf(TEXT("%s 返回 %s，不满足 expect：%s"), *Name, *Result.ToDisplay(200), *Check.Summary());
				}
			}
			return true;
		};
		// 超时、取消、守护插进来：接口自己收拾半路状态（只在没正常结束时调）
		InContext.PendingCleanup = [Pending]()
		{
			if (!Pending->bFinished && Pending->Call.Cleanup)
			{
				Pending->Call.Cleanup();
			}
		};
		InContext.Message = FString::Printf(TEXT("调 %s，等它完成"), *Name);
		return StepPending;
	}

	EGameTestStepStatus FinishScriptResult(FGameTestStepContext& InContext, const FString& InName, const FGameTestValue& InResult)
	{
		return GameTestScriptLeafLocal::FinishApi(InContext, InName, InResult);
	}

	bool HasScriptCond(const FJsonObject& InParams)
	{
		return InParams.HasField(TEXT("cond")) || InParams.HasField(TEXT("expr"));
	}

	/** assert 的 cond / expr（只看这一刻，不稳住）。`bOutHolds` 是结论，返回 false 表示没写这两个字段。 */
	bool CheckScriptAssert(FGameTestStepContext& InContext, bool& bOutHolds)
	{
		const FJsonObject& Params = *InContext.Step->Params;
		if (!HasScriptCond(Params))
		{
			return false;
		}
		if (InContext.Flow == nullptr)
		{
			bOutHolds = false;
			GameTestScriptLeafLocal::NoFlow(InContext, TEXT("assert cond / expr"));
			return true;
		}
		FGameTestFlowContext& Flow = *InContext.Flow;
		Flow.World = InContext.World ? InContext.World : Flow.World;
		// cond 和 expr 都写时两条都要成立：拼成 all
		TArray<TSharedPtr<FJsonValue>> Parts;
		if (const TSharedPtr<FJsonObject> Cond = GameTestCond::AsObject(Params.TryGetField(TEXT("cond"))))
		{
			Parts.Add(MakeShared<FJsonValueObject>(Cond));
		}
		FString Expr;
		if (Params.TryGetStringField(TEXT("expr"), Expr) && !Expr.IsEmpty())
		{
			const TSharedRef<FJsonObject> ExprCond = MakeShared<FJsonObject>();
			ExprCond->SetStringField(TEXT("expr"), Expr);
			Parts.Add(MakeShared<FJsonValueObject>(ExprCond));
		}
		if (Parts.Num() == 0)
		{
			bOutHolds = false;
			InContext.Message = TEXT("assert 的 cond 要写成条件对象、expr 要写成表达式字符串");
			return true;
		}
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		if (Parts.Num() == 1)
		{
			Root = Parts[0]->AsObject().ToSharedRef();
		}
		else
		{
			Root->SetArrayField(TEXT("all"), Parts);
		}
		FGameTestCondResult Result;
		{
			FGameTestFlowContext::FReadOnlyScope ReadOnly(Flow);
			Result = GameTestCond::Evaluate(*Root, Flow, nullptr);
		}
		InContext.Payload->SetObjectField(TEXT("cond"), Result.ToJson());
		bOutHolds = Result.Holds();
		InContext.Message = bOutHolds ? FString::Printf(TEXT("断言通过：%s"), *Result.Summary())
			: FString::Printf(TEXT("断言失败：%s"), *Result.Summary());
		return true;
	}

	EGameTestStepStatus StartWaitCond(FGameTestStepContext& InContext)
	{
		if (InContext.Flow == nullptr)
		{
			return GameTestScriptLeafLocal::NoFlow(InContext, TEXT("wait until=cond"));
		}
		const TSharedPtr<FJsonObject> Cond = GameTestCond::AsObject(InContext.Step->Params->TryGetField(TEXT("cond")));
		if (!Cond.IsValid())
		{
			InContext.Message = TEXT("wait until=cond 要写 cond（统一条件，见 docs/game-test-script.md 第 4 节）");
			return StepFailed;
		}
		return StartWaitFor(InContext, Cond.ToSharedRef());
	}

	EGameTestStepStatus StartWaitFor(FGameTestStepContext& InContext, const TSharedRef<FJsonObject>& InCond)
	{
		if (InContext.Flow == nullptr)
		{
			return GameTestScriptLeafLocal::NoFlow(InContext, TEXT("wait"));
		}
		const TSharedPtr<FJsonObject> Cond = InCond;
		FGameTestFlowContext* Flow = InContext.Flow;
		Flow->World = InContext.World ? InContext.World : Flow->World;
		// 只关心成立（稳住 settleMs），不成立不用稳住：等到超时为止
		const TSharedRef<FGameTestCondWatch> Watch = MakeShared<FGameTestCondWatch>();
		Watch->Start(Cond, *Flow, /*bNeedNegativeSettle=*/false);
		InContext.PendingProgress = MakeShared<FString>();
		const TSharedPtr<FString> Progress = InContext.PendingProgress;
		const TSharedPtr<FJsonObject> Payload = InContext.Payload;
		InContext.PendingPredicate = [Watch, Flow, Progress, Payload](double InNow)
		{
			FGameTestCondWatch::EVerdict Verdict = FGameTestCondWatch::EVerdict::Pending;
			{
				FGameTestFlowContext::FReadOnlyScope ReadOnly(*Flow);
				Verdict = Watch->Tick(*Flow, InNow);
			}
			*Progress = Watch->GetLast().Summary();
			if (Verdict == FGameTestCondWatch::EVerdict::Holds)
			{
				Payload->SetObjectField(TEXT("cond"), Watch->GetLast().ToJson());
				return true;
			}
			return false;
		};
		InContext.Message = TEXT("等条件成立");
		return StepPending;
	}
}
