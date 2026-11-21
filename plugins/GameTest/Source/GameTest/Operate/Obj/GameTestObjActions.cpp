// Copyright Epic Games, Inc. All Rights Reserved.

#include "Operate/Obj/GameTestObjActions.h"

#include "Core/Expr/GameTestExpr.h"
#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestRunner.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Ref/GameTestPropPath.h"
#include "Observe/Ref/GameTestRef.h"
#include "Observe/Ref/GameTestRefResolve.h"
#include "Operate/Close/GameTestCloser.h"

#include "Components/Widget.h"

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

	/**
	 * each：引用命中的全部（ref_ambiguous 时 Matches 也是全的；`$var`、来源直接给的只有 Picked），每个按 part 找部件。
	 * 有一个找不到部件就返回空、OutCode = part_missing；没显示出来的 ListView 条目跳过（没有控件可调）。
	 */
	TArray<UObject*> AllParts(const FGameTestTargetResult& InResult, FName& OutCode, FString& OutError)
	{
		TArray<UObject*> Owners;
		for (const FGameTestRefCandidate& Candidate : InResult.Ref.Matches)
		{
			if (UObject* Object = Candidate.Object.Get())
			{
				Owners.AddUnique(Object);
			}
		}
		if (Owners.Num() == 0 && InResult.Ref.Get() != nullptr)
		{
			Owners.Add(InResult.Ref.Get());
		}
		TArray<UObject*> Parts;
		for (UObject* Owner : Owners)
		{
			FString PartError;
			UObject* Part = GameTestPropRead::FindPart(Owner, InResult.Target.Part, PartError);
			if (Part == nullptr)
			{
				OutCode = TEXT("part_missing");
				OutError = FString::Printf(TEXT("%s：%s"), *GameTestRefResolve::DisplayName(Owner), *PartError);
				return TArray<UObject*>();
			}
			Parts.Add(Part);
		}
		if (Parts.Num() == 0)
		{
			OutCode = TEXT("ref_not_found");
			OutError = InResult.Error.IsEmpty() ? FString(TEXT("一个都没找到")) : InResult.Error;
		}
		return Parts;
	}

	EGameTestStepStatus RunWhenFoundImpl(FGameTestStepContext& InContext, const FString& InWhat, bool bInAll, GameTestObjActions::FDoAll InDo)
	{
		if (InContext.Flow == nullptr)
		{
			return GameTestObjActions::NoFlow(InContext, *InContext.Step->Action.ToString());
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
		auto Attempt = [Target, bInAll](FGameTestStepContext& InStep, bool& bOutRetry) -> TArray<UObject*>
		{
			FGameTestFlowContext& StepFlow = *InStep.Flow;
			const TSharedPtr<FJsonObject> Prev = StepFlow.StepPayload;
			StepFlow.StepPayload = InStep.Payload;
			const FGameTestTargetResult Result = GameTestRefResolve::ResolveTarget(Target, StepFlow);
			GameTestRefResolve::WritePayload(Result.Ref, StepFlow);
			StepFlow.StepPayload = Prev;
			TArray<UObject*> Found;
			FName Code = Result.Code;
			FString Error = Result.Error;
			if (bInAll && (Result.IsOk() || Result.Code == TEXT("ref_ambiguous")))
			{
				Found = AllParts(Result, Code, Error);
				// each 时命中多个不是错：payload.ref 改成「挑了全部」
				const TSharedPtr<FJsonObject>* RefPayload = nullptr;
				if (Found.Num() > 0 && InStep.Payload.IsValid() && InStep.Payload->TryGetObjectField(TEXT("ref"), RefPayload) && RefPayload != nullptr && RefPayload->IsValid())
				{
					(*RefPayload)->RemoveField(TEXT("error"));
					(*RefPayload)->RemoveField(TEXT("code"));
					(*RefPayload)->SetStringField(TEXT("by"), TEXT("each"));
					(*RefPayload)->SetNumberField(TEXT("count"), Found.Num());
				}
			}
			else if (Result.IsOk())
			{
				Found.Add(Result.Get());
			}
			bOutRetry = Found.Num() == 0 && (Code == TEXT("ref_not_found") || Code == TEXT("part_missing"));
			if (Found.Num() == 0)
			{
				InStep.Message = FString::Printf(TEXT("%s：%s"), *Code.ToString(), *Error);
				InStep.Payload->SetStringField(TEXT("code"), Code.ToString());
				return Found;
			}
			InStep.TouchedWidget = Cast<UWidget>(Found[0]);
			return Found;
		};

		bool bRetry = false;
		{
			const TArray<UObject*> Found = Attempt(InContext, bRetry);
			if (Found.Num() > 0)
			{
				return InDo(InContext, Found);
			}
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
			const TArray<UObject*> Found = Attempt(*Copy, bAgain);
			if (Found.Num() == 0)
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
			CheckString(InJson, TEXT("prop"), InSink, TEXT("属性路径，如 @.Count、@/ChkAuto.IsChecked"));
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
			const TSharedPtr<FJsonValue> Each = InJson.TryGetField(TEXT("each"));
			if (Each.IsValid() && Each->Type != EJson::Boolean)
			{
				InSink.Error(TEXT("bad_type"), TEXT("each 要写 true / false（true = 引用命中几个就每个都调）"));
			}
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
		return GameTestObjActionsImpl::RunWhenFoundImpl(InContext, InWhat, false, [InDo](FGameTestStepContext& InStep, const TArray<UObject*>& InObjects)
		{
			return InDo(InStep, InObjects[0]);
		});
	}

	EGameTestStepStatus RunWhenFoundAll(FGameTestStepContext& InContext, const FString& InWhat, FDoAll InDo)
	{
		return GameTestObjActionsImpl::RunWhenFoundImpl(InContext, InWhat, true, MoveTemp(InDo));
	}
}
