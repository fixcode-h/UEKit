// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestRunner.h"

#include "Core/GameTestAction.h"
#include "Core/GameTestWorld.h"
#include "Observe/GameTestWidgetNode.h"

#include "Misc/DateTime.h"

bool FGameTestRunner::StartRun(
	const FGameTestRunRequest& InRequest,
	const FGameTestRuntimeState& InRuntimeState,
	FGameTestOnFinished InOnFinished,
	FString& OutError)
{
	if (bActive)
	{
		OutError = TEXT("已有一个 run 在进行中；等它结束或先 stop");
		return false;
	}

	if (InRequest.Steps.Num() == 0)
	{
		OutError = TEXT("没有步骤可执行");
		return false;
	}

	Report = FGameTestReport();
	Report.RunId = InRequest.RunId;
	Report.Name = InRequest.Name;
	Report.Scenario = InRequest.Scenario;
	Report.Revision = InRequest.Revision;
	Report.StartedAtUtc = FDateTime::UtcNow().ToString(TEXT("%Y-%m-%dT%H:%M:%SZ"));
	StartedAtSeconds = FPlatformTime::Seconds();

	Queue = InRequest.Steps;
	CurrentIndex = 0;
	CurrentRecord = FGameTestStepRecord();
	PendingPredicate = nullptr;
	OnFinished = MoveTemp(InOnFinished);
	OutputDir = InRequest.OutputDir;
	DefaultStepTimeoutMs = InRequest.DefaultStepTimeoutMs;
	StepDelayMs = InRequest.StepDelayMs;
	Handles = InRuntimeState.Handles;
	RootedObjects = InRuntimeState.RootedObjects;
	OnStepFailed = InRuntimeState.OnStepFailed;
	bCancelling = false;

	bActive = true;
	bStepInFlight = false;
	bHasReport = false;
	NextStepNotBefore = 0.0;
	TouchedWidget.Reset();
	bTouchedSucceeded = false;
	return true;
}

void FGameTestRunner::Tick(double InNowSeconds)
{
	if (!bActive)
	{
		return;
	}

	// 一帧内把"能立刻做完的步骤"连着跑完：编辑器被节流时一帧可能隔很久才来
	// （窗口不在前台时引擎默认节流），一步一帧会让整场景停摆。
	// 只有跨帧动作（Pending）才真的停下来等下一帧；上限则保证编辑器仍能喘气。
	//
	// 步间隔（StepDelayMs）：上一步成功后先停这么久再起步——让 UMG 把这一帧刷完、
	// 也让人看清高亮的那个控件。只延迟"还有下一步"的情况，所以它不会挡住收尾。
	if (!bStepInFlight && InNowSeconds < NextStepNotBefore)
	{
		return;
	}

	constexpr int32 MaxStepsPerTick = 64;
	int32 StepsThisTick = 0;

	while (bActive && !bStepInFlight && CurrentIndex < Queue.Num()
		&& StepsThisTick < MaxStepsPerTick && InNowSeconds >= NextStepNotBefore)
	{
		StartCurrentStep(InNowSeconds);
		++StepsThisTick;
	}

	// 跨帧动作：本帧求值一次谓词。完成之后**不要在这里 return**——
	// 要接着往下走，让"队列已空"能在同一帧收尾（否则整次 run 会晚一帧才结束，
	// 而 job 快照可能正好卡在"已完成但还显示 running"的那一帧上）。
	if (bActive && bStepInFlight)
	{
		if (PendingPredicate.IsSet() && PendingPredicate(InNowSeconds))
		{
			if (PendingFailure.IsValid() && !PendingFailure->IsEmpty())
			{
				CompleteCurrentStep(EGameTestStepStatus::Failed, *PendingFailure, InNowSeconds);
			}
			else
			{
				CompleteCurrentStep(EGameTestStepStatus::Succeeded,
					FString::Printf(TEXT("%s（已完成，用时 %.0f ms）"),
						*CurrentRecord.Message, (InNowSeconds - CurrentRecord.StartedAt) * 1000.0),
					InNowSeconds);
			}
		}
		else if (InNowSeconds >= StepDeadline)
		{
			// 超时时把谓词留下的进度拼进去："还差多远 / 卡在哪"是超时唯一有用的线索。
			const FString Progress = PendingProgress.IsValid() ? *PendingProgress : FString();
			CompleteCurrentStep(EGameTestStepStatus::Failed,
				FString::Printf(TEXT("%s%s；等待超时（%d ms）"),
					*CurrentRecord.Message,
					Progress.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("；%s"), *Progress),
					CurrentRecord.Step.TimeoutMs),
				InNowSeconds);
		}
	}

	if (bActive && !bStepInFlight && CurrentIndex >= Queue.Num())
	{
		FinishRun();
	}
}

void FGameTestRunner::StartCurrentStep(double InNowSeconds)
{
	const FGameTestStep& Step = Queue[CurrentIndex];

	CurrentRecord = FGameTestStepRecord();
	CurrentRecord.Step = Step;
	CurrentRecord.StartedAt = InNowSeconds;

	const int32 TimeoutMs = Step.TimeoutMs > 0 ? Step.TimeoutMs : DefaultStepTimeoutMs;

	// 超时值写回记录：消息里要能报出真正的上限。
	CurrentRecord.Step.TimeoutMs = TimeoutMs;
	StepDeadline = InNowSeconds + TimeoutMs / 1000.0;
	PendingPredicate = nullptr;
	PendingFailure.Reset();

	FGameTestStepContext Context;
	Context.Step = &CurrentRecord.Step;
	Context.World = GameTestWorld::FindGameWorld();
	Context.Handles = Handles;
	Context.RootedObjects = RootedObjects;
	Context.OutputDir = OutputDir;

	EGameTestStepStatus Status = EGameTestStepStatus::Failed;

	if (!CurrentRecord.Step.Params.IsValid())
	{
		Context.Message = TEXT("步骤没有参数对象（解析阶段就该拦住，这是缺陷）");
	}
	else if (const FGameTestHandler Handler = GameTestActions::FindHandler(CurrentRecord.Step.Action))
	{
		Status = Handler(Context);
	}
	else
	{
		// 解析之后提供它的可选模块被卸载了（极少见）：如实报出来，不当成用户写错。
		Context.Message = FString::Printf(TEXT("动作 %s 已经没有实现（提供它的模块在 run 期间被卸载了）"), *CurrentRecord.Step.Action.ToString());
	}

	CurrentRecord.Payload = Context.Payload;
	CurrentRecord.Message = Context.Message;

	// 记住"这一步碰的是谁"并 bump 序号：视口高亮据此把控件框出来（成功失败都框）。
	RecordTouch(Context);

	if (Status == EGameTestStepStatus::Pending)
	{
		if (!Context.PendingPredicate.IsSet())
		{
			CompleteCurrentStep(EGameTestStepStatus::Failed,
				FString::Printf(TEXT("%s：动作返回 Pending 却没有给等待谓词（实现缺陷）"), *Context.Message), InNowSeconds);
			return;
		}

		PendingPredicate = MoveTemp(Context.PendingPredicate);
		PendingProgress = Context.PendingProgress;
		PendingFailure = Context.PendingFailure;
		PendingCleanup = MoveTemp(Context.PendingCleanup);

		// 步骤没写 timeoutMs、动作又给了自己的默认值：按动作的来（UI 自动等待默认短一些）。
		if (Step.TimeoutMs <= 0 && Context.DefaultTimeoutMs > 0)
		{
			CurrentRecord.Step.TimeoutMs = Context.DefaultTimeoutMs;
			StepDeadline = InNowSeconds + Context.DefaultTimeoutMs / 1000.0;
		}
		bStepInFlight = true;
		return;
	}

	CompleteCurrentStep(Status, Context.Message, InNowSeconds);
}

void FGameTestRunner::RecordTouch(const FGameTestStepContext& InContext)
{
	const bool bHasTarget = InContext.TouchedWidget.IsValid();
	const bool bChanged = bHasTarget && TouchedWidget.Get() != InContext.TouchedWidget.Get();

	TouchedWidget = InContext.TouchedWidget;
	bTouchedHasRect = InContext.bTouchedHasRect;
	TouchedMin = InContext.TouchedMin;
	TouchedMax = InContext.TouchedMax;

	// 只有真的换了控件才 bump：同一步里的重复解析（快照、断言各解析一次）不该被当作新动作。
	if (bChanged || (bHasTarget && TouchSerial == 0))
	{
		++TouchSerial;
	}
}

void FGameTestRunner::CompleteCurrentStep(EGameTestStepStatus InStatus, const FString& InMessage, double InNowSeconds)
{
	CurrentRecord.Status = InStatus;
	CurrentRecord.Message = InMessage;
	CurrentRecord.FinishedAt = InNowSeconds;
	bTouchedSucceeded = (InStatus == EGameTestStepStatus::Succeeded);

	const bool bStopAfterFailure = (InStatus == EGameTestStepStatus::Failed)
		&& CurrentRecord.Step.FailMode == EGameTestFailMode::Stop;

	// 收尾放在留证据之前：真实点击超时时鼠标键还按着，截图里不该带着这个半截状态，日志也该先写完收尾那几行。
	bStepInFlight = false;
	PendingPredicate = nullptr;
	if (PendingCleanup)
	{
		// 先挪出来再调：收尾里发的输入不该再碰到执行器的这个成员。
		const TFunction<void()> Cleanup = MoveTemp(PendingCleanup);
		PendingCleanup = nullptr;
		Cleanup();
	}

	if (InStatus == EGameTestStepStatus::Failed && !bCancelling && OnStepFailed)
	{
		OnStepFailed(CurrentRecord, OutputDir);
	}

	Report.Steps.Add(CurrentRecord);
	++CurrentIndex;

	if (bStopAfterFailure)
	{
		FinishRun();
		return;
	}

	// 成功才停：失败要么已经收尾（Stop），要么让人立刻看到现场（Continue），都不该再等。
	if (InStatus == EGameTestStepStatus::Succeeded && StepDelayMs > 0 && CurrentIndex < Queue.Num())
	{
		NextStepNotBefore = InNowSeconds + StepDelayMs / 1000.0;
	}
}

void FGameTestRunner::Cancel(const FString& InReason)
{
	if (!bActive)
	{
		return;
	}

	// 只有**真的有步骤在执行**时才把它标失败。取消恰好落在两步之间（步间隔里就是这个窗口，
	// 默认 1 秒）时没有任何步骤在执行：此时不能去完成 CurrentRecord——那里面躺着的是**上一步
	// 已记入报告**的记录，再 Add 一次会让报告出现重复条目、把上一步的成功改写成失败，而中间
	// 那一步从报告里彻底消失（步数与场景对不上）。剩余步骤统一交给 FinishRun 标 skipped。
	if (bStepInFlight)
	{
		bCancelling = true;
		CompleteCurrentStep(EGameTestStepStatus::Failed,
			FString::Printf(TEXT("已取消：%s"), *InReason), FPlatformTime::Seconds());
	}

	FinishRun();
}

void FGameTestRunner::FinishRun()
{
	// 幂等：CompleteCurrentStep 在"失败即停"时已经收过一次尾（取消正在执行的步骤走的正是这条路），
	// 收过就不再重复——否则会重算时间戳、并再触发一次完成回调。
	if (!bActive)
	{
		return;
	}

	// 没轮到执行的步骤如实标成跳过，而不是从报告里消失。
	for (int32 Index = CurrentIndex; Index < Queue.Num(); ++Index)
	{
		FGameTestStepRecord Skipped;
		Skipped.Step = Queue[Index];
		Skipped.Status = EGameTestStepStatus::Skipped;
		Skipped.Message = TEXT("未执行（前一步失败或整次 run 被取消）");
		Skipped.StartedAt = 0.0;
		Skipped.FinishedAt = 0.0;
		Report.Steps.Add(Skipped);
	}

	Report.FinishedAtUtc = FDateTime::UtcNow().ToString(TEXT("%Y-%m-%dT%H:%M:%SZ"));
	Report.DurationMs = (FPlatformTime::Seconds() - StartedAtSeconds) * 1000.0;
	Report.bSucceeded = Report.CountByStatus(EGameTestStepStatus::Failed) == 0
		&& Report.CountByStatus(EGameTestStepStatus::Skipped) == 0;

	bActive = false;
	bStepInFlight = false;
	bHasReport = true;
	CurrentIndex = Queue.Num();
	NextStepNotBefore = 0.0;

	if (OnFinished)
	{
		// 先搬出来再调用：回调里如果再起一次 run，不能把这次的闭包覆盖掉。
		const FGameTestOnFinished Callback = MoveTemp(OnFinished);
		OnFinished = nullptr;
		Callback(Report);
	}
}
