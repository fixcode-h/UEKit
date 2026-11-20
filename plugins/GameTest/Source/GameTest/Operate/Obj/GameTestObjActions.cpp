// Copyright Epic Games, Inc. All Rights Reserved.

#include "Operate/Obj/GameTestObjActions.h"

#include "Core/Expr/GameTestExpr.h"
#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestRunner.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Ref/GameTestRef.h"
#include "Observe/Ref/GameTestRefResolve.h"
#include "Operate/Close/GameTestCloser.h"

namespace GameTestObjActionsImpl
{
	/** obj 必填并且是引用（obj.call 另外接受 `@来源`、`@luaMod.模块`，也可以省略）。 */
	void CheckObj(const FJsonObject& InJson, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink, bool bInCall)
	{
		const TSharedPtr<FJsonValue> Obj = InJson.TryGetField(TEXT("obj"));
		if (!Obj.IsValid() || Obj->IsNull())
		{
			if (!bInCall)
			{
				InSink.Error(TEXT("arg_missing"), TEXT("要写 obj（objects 里的名字、\"$变量\" 或 {class, ident?}）"));
			}
			return;
		}
		FString Text;
		if (bInCall && Obj->TryGetString(Text) && Text.StartsWith(TEXT("@")))
		{
			// `@luaMod.模块`、`@sub.X` 这类取值来源：由来源注册表在运行时解析
			return;
		}
		FGameTestDiagSink::FScope Scope(InSink, TEXT("obj"));
		GameTestRefCheck::Ref(Obj, InScope, InSink);
	}

	void CheckString(const FJsonObject& InJson, const TCHAR* InField, FGameTestDiagSink& InSink, const TCHAR* InWhat)
	{
		FString Text;
		if (!InJson.TryGetStringField(InField, Text) || Text.IsEmpty())
		{
			InSink.Error(TEXT("arg_missing"), FString::Printf(TEXT("要写 %s（%s）"), InField, InWhat));
		}
	}

	/** obj.close 的 how / verify / verifyMs。 */
	void CheckClose(const FJsonObject& InJson, FGameTestDiagSink& InSink)
	{
		const TSharedPtr<FJsonValue> How = InJson.TryGetField(TEXT("how"));
		if (How.IsValid() && !How->IsNull())
		{
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!How->TryGetArray(Items) || Items == nullptr || Items->Num() == 0)
			{
				InSink.Error(TEXT("bad_type"), TEXT("how 要写关闭方式的数组：[{\"by\": \"call\", \"function\": \"Close\"}, {\"by\": \"remove\"}]"));
			}
			else
			{
				for (int32 Index = 0; Index < Items->Num(); ++Index)
				{
					FGameTestDiagSink::FScope Scope(InSink, FString::Printf(TEXT("how.%d"), Index));
					GameTestClosers::ValidateHow((*Items)[Index], InSink);
				}
			}
		}
		FString Verify;
		if (InJson.TryGetStringField(TEXT("verify"), Verify) && Verify != TEXT("gone") && Verify != TEXT("hidden") && Verify != TEXT("none"))
		{
			InSink.Error(TEXT("bad_type"), FString::Printf(TEXT("verify 只能是 gone / hidden / none，写的是 %s"), *Verify));
		}
		double VerifyMs = 0.0;
		if (InJson.HasField(TEXT("verifyMs")) && (!InJson.TryGetNumberField(TEXT("verifyMs"), VerifyMs) || VerifyMs < 0.0 || VerifyMs > 600000.0))
		{
			InSink.Error(TEXT("bad_type"), TEXT("verifyMs 要写 0–600000 的毫秒数"));
		}
	}
}

namespace GameTestObjActions
{
	void Validate(const FJsonObject& InJson, FName InAction, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink)
	{
		using namespace GameTestObjActionsImpl;
		const FString Action = InAction.ToString();
		if (!Action.StartsWith(TEXT("obj.")))
		{
			return;
		}
		CheckObj(InJson, InScope, InSink, Action == TEXT("obj.call"));
		if (Action == TEXT("obj.find"))
		{
			CheckString(InJson, TEXT("saveAs"), InSink, TEXT("存到哪个变量"));
		}
		else if (Action == TEXT("obj.wait"))
		{
			GameTestRefCheck::Ident(InJson, InScope, InSink, /*bInPropRequired=*/false);
		}
		else if (Action == TEXT("obj.set"))
		{
			CheckString(InJson, TEXT("prop"), InSink, TEXT("属性路径，如 @self.Count、@ChkAuto.IsChecked"));
			FString Prop;
			if (InJson.TryGetStringField(TEXT("prop"), Prop) && !Prop.IsEmpty())
			{
				GameTestRefCheck::Prop(Prop, InSink);
			}
			if (!InJson.HasField(TEXT("value")))
			{
				InSink.Error(TEXT("arg_missing"), TEXT("obj.set 要写 value"));
			}
		}
		else if (Action == TEXT("obj.call"))
		{
			CheckString(InJson, TEXT("function"), InSink, TEXT("函数名；静态函数写 类名.函数"));
		}
		else if (Action == TEXT("obj.close"))
		{
			CheckClose(InJson, InSink);
		}
	}

	EGameTestStepStatus NoFlow(FGameTestStepContext& InContext, const TCHAR* InAction)
	{
		InContext.Message = FString::Printf(TEXT("这一步没有脚本上下文（%s 只能在场景里用）"), InAction);
		return GameTestHandlers::StepFailed;
	}

	EGameTestStepStatus RunWhenFound(FGameTestStepContext& InContext, const FString& InWhat, FDo InDo)
	{
		if (InContext.Flow == nullptr)
		{
			return NoFlow(InContext, *InContext.Step->Action.ToString());
		}
		FGameTestFlowContext& Flow = *InContext.Flow;
		Flow.World = InContext.World ? InContext.World : Flow.World;
		const FJsonObject& Params = *InContext.Step->Params;
		const TSharedRef<FJsonObject> TargetJson = MakeShared<FJsonObject>();
		TargetJson->SetField(TEXT("obj"), Params.TryGetField(TEXT("obj")));
		FString Part;
		if (Params.TryGetStringField(TEXT("part"), Part) && !Part.IsEmpty())
		{
			TargetJson->SetStringField(TEXT("part"), Part);
		}
		const TSharedRef<FJsonValueObject> Target = MakeShared<FJsonValueObject>(TargetJson);

		// 解析一次：成了马上做；等也没用的直接失败；还没出现的每帧再找
		auto Attempt = [Target](FGameTestStepContext& InStep, bool& bOutRetry) -> UObject*
		{
			FGameTestFlowContext& StepFlow = *InStep.Flow;
			const TSharedPtr<FJsonObject> Prev = StepFlow.StepPayload;
			StepFlow.StepPayload = InStep.Payload;
			const FGameTestTargetResult Result = GameTestRefResolve::ResolveTarget(Target, StepFlow);
			GameTestRefResolve::WritePayload(Result.Ref, StepFlow);
			StepFlow.StepPayload = Prev;
			bOutRetry = !Result.IsOk() && (Result.Code == TEXT("ref_not_found") || Result.Code == TEXT("part_missing"));
			if (!Result.IsOk())
			{
				InStep.Message = FString::Printf(TEXT("%s：%s"), *Result.Code.ToString(), *Result.Error);
				InStep.Payload->SetStringField(TEXT("code"), Result.Code.ToString());
				return nullptr;
			}
			InStep.TouchedWidget = Result.GetWidget();
			return Result.Get();
		};

		bool bRetry = false;
		if (UObject* Found = Attempt(InContext, bRetry))
		{
			return InDo(InContext, Found);
		}
		if (!bRetry)
		{
			return GameTestHandlers::StepFailed;
		}

		// 还没出现：和点击的自动等待同一个上限（默认不限，步骤 timeoutMs 优先）
		InContext.DefaultTimeoutMs = InContext.UiWaitDefault();
		InContext.PendingFailure = MakeShared<FString>();
		InContext.PendingProgress = MakeShared<FString>();
		const TSharedRef<FGameTestStepContext> Copy = MakeShared<FGameTestStepContext>(InContext);
		const TSharedPtr<FString> Failure = InContext.PendingFailure;
		const TSharedPtr<FString> Progress = InContext.PendingProgress;
		const TSharedRef<double> LastTry = MakeShared<double>(-1.0);
		InContext.PendingPredicate = [Copy, Attempt, InDo, Failure, Progress, LastTry](double InNow)
		{
			// 解析要采界面树：隔 0.1 秒找一次就够
			if (*LastTry >= 0.0 && InNow - *LastTry < 0.1)
			{
				return false;
			}
			*LastTry = InNow;
			bool bAgain = false;
			UObject* Found = Attempt(*Copy, bAgain);
			if (Found == nullptr)
			{
				*Progress = Copy->Message;
				if (bAgain)
				{
					return false;
				}
				*Failure = Copy->Message;
				return true;
			}
			if (InDo(*Copy, Found) != GameTestHandlers::StepSucceeded)
			{
				*Failure = Copy->Message.IsEmpty() ? FString(TEXT("失败（没有说明）")) : Copy->Message;
			}
			return true;
		};
		FGameTestTarget Described;
		FGameTestTarget::FromJson(Target, Described);
		InContext.Message = FString::Printf(TEXT("等 %s 出现后%s"), *Described.Describe(), *InWhat);
		return GameTestHandlers::StepPending;
	}
}
