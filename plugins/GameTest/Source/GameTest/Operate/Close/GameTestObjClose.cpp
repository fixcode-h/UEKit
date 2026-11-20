// Copyright Epic Games, Inc. All Rights Reserved.

// obj.close（docs/game-test-ref.md 5.2）：按 how 的顺序试关闭方式，每种做完按 verify 核对（跨帧，最多 verifyMs），
// 没过就试下一种；全没过这一步失败（close_failed）。报告 payload.close: [{by, ok, ms, message?}]。
// 关闭方式本身在注册表里（Operate/Close/GameTestClose*.cpp），这里只管顺序、核对和报告。

#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestRunner.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Ref/GameTestPropPath.h"
#include "Observe/Ref/GameTestRefResolve.h"
#include "Operate/Close/GameTestCloser.h"
#include "Operate/Obj/GameTestObjActions.h"

namespace GameTestObjCloseImpl
{
	/** 解析、核对隔这么久做一次（要采界面树）。 */
	constexpr double PollSeconds = 0.1;

	enum class EPhase : uint8
	{
		Find,
		Close,
		Verify,
	};

	struct FState
	{
		TSharedPtr<FJsonValue> Obj;
		TArray<TSharedPtr<FJsonObject>> Hows;
		bool bHowGiven = false;
		FString Verify;
		double VerifyMs = 2000.0;
		EPhase Phase = EPhase::Find;
		int32 Index = 0;
		double PhaseStart = 0.0;
		double LastPoll = -1.0;
		double CloseStart = 0.0;
		TArray<TSharedPtr<FJsonValue>> Log;
		FString From;
	};

	/** 关掉了没有：gone = 引用找不到（`$var` 时看 :onScreen，变量里的弱引用在界面移除后还活着）；hidden = :onScreen 为 false。 */
	bool Verified(FState& InState, FGameTestFlowContext& InFlow)
	{
		if (InState.Verify == TEXT("none"))
		{
			return true;
		}
		const FGameTestRefResult Result = GameTestRefResolve::Resolve(InState.Obj, InFlow);
		if (!Result.IsOk())
		{
			return Result.Code == TEXT("ref_not_found");
		}
		if (InState.Verify == TEXT("gone") && Result.By != TEXT("var"))
		{
			return false;
		}
		const FGameTestPropRead Read = GameTestPropRead::ReadText(Result.Get(), TEXT(":onScreen"), InFlow, Result.GetItem());
		return Read.IsResolved() && !Read.Value.Truthy();
	}

	void AddLog(FState& InState, const FString& InBy, bool bInOk, double InMs, const FString& InMessage)
	{
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("by"), InBy);
		Entry->SetBoolField(TEXT("ok"), bInOk);
		Entry->SetNumberField(TEXT("ms"), FMath::RoundToInt(InMs * 1000.0));
		if (!InMessage.IsEmpty())
		{
			Entry->SetStringField(TEXT("message"), InMessage);
		}
		InState.Log.Add(MakeShared<FJsonValueObject>(Entry));
	}

	FString Summary(const FState& InState)
	{
		TArray<FString> Lines;
		for (const TSharedPtr<FJsonValue>& Value : InState.Log)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (Value.IsValid() && Value->TryGetObject(Entry) && Entry != nullptr && Entry->IsValid())
			{
				FString By;
				FString Message;
				(*Entry)->TryGetStringField(TEXT("by"), By);
				(*Entry)->TryGetStringField(TEXT("message"), Message);
				Lines.Add(FString::Printf(TEXT("%s：%s"), *By, *Message));
			}
		}
		return FString::Join(Lines, TEXT("；"));
	}

	/** 走一帧。返回 true = 结束（成功或失败写进 OutFailure）。 */
	bool Tick(FState& InState, FGameTestStepContext& InStep, double InNow, FString& OutFailure, FString& OutProgress)
	{
		FGameTestFlowContext& Flow = *InStep.Flow;
		InStep.Payload->SetArrayField(TEXT("close"), InState.Log);
		if (InState.LastPoll >= 0.0 && InNow - InState.LastPoll < PollSeconds && InState.Phase != EPhase::Close)
		{
			return false;
		}
		InState.LastPoll = InNow;

		if (InState.Phase == EPhase::Find)
		{
			const TSharedPtr<FJsonObject> PrevPayload = Flow.StepPayload;
			Flow.StepPayload = InStep.Payload;
			const FGameTestRefResult Result = GameTestRefResolve::Resolve(InState.Obj, Flow);
			GameTestRefResolve::WritePayload(Result, Flow);
			Flow.StepPayload = PrevPayload;
			if (!Result.IsOk())
			{
				OutProgress = FString::Printf(TEXT("%s：%s"), *Result.Code.ToString(), *Result.Error);
				if (Result.Code != TEXT("ref_not_found"))
				{
					OutFailure = OutProgress;
					return true;
				}
				return false;
			}
			if (!InState.bHowGiven)
			{
				FString RuleVerify;
				InState.Hows = GameTestClosers::DefaultHows(Result.Get(), Flow, RuleVerify, InState.From);
				if (InState.Verify.IsEmpty())
				{
					InState.Verify = RuleVerify;
				}
				InStep.Payload->SetStringField(TEXT("closeFrom"), InState.From);
			}
			if (InState.Verify.IsEmpty())
			{
				InState.Verify = TEXT("gone");
			}
			if (InState.Hows.Num() == 0)
			{
				OutFailure = FString::Printf(TEXT("close_unavailable：%s 没有能用的关闭方式（写 how，或在 _settings.json 的 closeRules 里给这个类配一条）"),
					*GameTestRefResolve::DisplayName(Result.Get()));
				return true;
			}
			InState.Phase = EPhase::Close;
		}

		if (InState.Phase == EPhase::Close)
		{
			if (!InState.Hows.IsValidIndex(InState.Index))
			{
				OutFailure = FString::Printf(TEXT("close_failed：试过的关闭方式都没关掉（%s）"), *Summary(InState));
				return true;
			}
			const TSharedPtr<FJsonObject> How = InState.Hows[InState.Index];
			FString By;
			How->TryGetStringField(TEXT("by"), By);
			const IGameTestCloser* Closer = GameTestClosers::Get().Find(FName(*By));
			// 每种方式开始前重新解析：上一种可能已经把对象换掉了（关了又开、列表刷新）
			const FGameTestRefResult Result = GameTestRefResolve::Resolve(InState.Obj, Flow);
			if (!Result.IsOk())
			{
				// 上一种方式核对超时以后对象才没了：也算关掉了
				AddLog(InState, By, true, 0.0, TEXT("开始前对象已经不在了"));
				InStep.Payload->SetArrayField(TEXT("close"), InState.Log);
				return true;
			}
			if (Closer == nullptr)
			{
				AddLog(InState, By, false, 0.0, By == TEXT("pop") ? TEXT("close_unavailable：by=pop 要 GameTestCommonUI 模块（启用 CommonUI）") : TEXT("close_unavailable：不认识这种关闭方式"));
				++InState.Index;
				return false;
			}
			FGameTestCloseRun Run(InStep, Flow, Result.Get(), *How);
			const double Start = FPlatformTime::Seconds();
			const bool bDone = Closer->Close(Run);
			if (!bDone)
			{
				AddLog(InState, By, false, FPlatformTime::Seconds() - Start, Run.Message);
				++InState.Index;
				return false;
			}
			InState.CloseStart = InNow;
			InState.Phase = EPhase::Verify;
			InStep.Payload->SetStringField(TEXT("closing"), Run.Message);
			OutProgress = FString::Printf(TEXT("%s，等核对（%s）"), *Run.Message, *InState.Verify);
			return false;
		}

		// 核对
		FString By;
		InState.Hows[InState.Index]->TryGetStringField(TEXT("by"), By);
		FString Did;
		InStep.Payload->TryGetStringField(TEXT("closing"), Did);
		if (Verified(InState, Flow))
		{
			AddLog(InState, By, true, InNow - InState.CloseStart, Did);
			InStep.Payload->RemoveField(TEXT("closing"));
			InStep.Payload->SetArrayField(TEXT("close"), InState.Log);
			return true;
		}
		if ((InNow - InState.CloseStart) * 1000.0 < InState.VerifyMs)
		{
			return false;
		}
		AddLog(InState, By, false, InNow - InState.CloseStart, FString::Printf(TEXT("%s，但 %.0f 毫秒内没核对到 %s"), *Did, InState.VerifyMs, *InState.Verify));
		InStep.Payload->RemoveField(TEXT("closing"));
		++InState.Index;
		InState.Phase = EPhase::Close;
		return false;
	}
}

namespace GameTestHandlers
{
	EGameTestStepStatus HandleObjClose(FGameTestStepContext& InContext)
	{
		using namespace GameTestObjCloseImpl;
		if (InContext.Flow == nullptr)
		{
			return GameTestObjActions::NoFlow(InContext, TEXT("obj.close"));
		}
		FGameTestFlowContext& Flow = *InContext.Flow;
		Flow.World = InContext.World ? InContext.World : Flow.World;
		const FJsonObject& Params = *InContext.Step->Params;

		const TSharedRef<FState> State = MakeShared<FState>();
		State->Obj = Params.TryGetField(TEXT("obj"));
		const TArray<TSharedPtr<FJsonValue>>* Hows = nullptr;
		if (Params.TryGetArrayField(TEXT("how"), Hows) && Hows != nullptr)
		{
			State->bHowGiven = true;
			State->From = TEXT("how");
			for (const TSharedPtr<FJsonValue>& Value : *Hows)
			{
				const TSharedPtr<FJsonObject>* How = nullptr;
				if (Value.IsValid() && Value->TryGetObject(How) && How != nullptr && How->IsValid())
				{
					State->Hows.Add(*How);
				}
			}
		}
		Params.TryGetStringField(TEXT("verify"), State->Verify);
		State->VerifyMs = Flow.Options.CloseVerifyMs;
		Params.TryGetNumberField(TEXT("verifyMs"), State->VerifyMs);
		State->PhaseStart = FPlatformTime::Seconds();

		// 对象还没出现时和点击的自动等待同一个上限；核对另有 verifyMs
		InContext.DefaultTimeoutMs = InContext.UiWaitDefault();
		InContext.PendingFailure = MakeShared<FString>();
		InContext.PendingProgress = MakeShared<FString>();
		const TSharedRef<FGameTestStepContext> Copy = MakeShared<FGameTestStepContext>(InContext);
		const TSharedPtr<FString> Failure = InContext.PendingFailure;
		const TSharedPtr<FString> Progress = InContext.PendingProgress;
		InContext.PendingPredicate = [State, Copy, Failure, Progress](double InNow)
		{
			// 一帧里最多试几种（做不了的方式不用等到下一帧）
			for (int32 Guard = 0; Guard < 8; ++Guard)
			{
				const int32 Before = State->Index;
				const EPhase PhaseBefore = State->Phase;
				if (Tick(*State, *Copy, InNow, *Failure, *Progress))
				{
					return true;
				}
				if (State->Phase == PhaseBefore && State->Index == Before)
				{
					return false;
				}
			}
			return false;
		};
		InContext.Message = TEXT("关掉对象");
		return StepPending;
	}
}
