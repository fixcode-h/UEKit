// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/GameTestAction.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestRunner.h"
#include "Core/GameTestTypes.h"

#include "Dom/JsonObject.h"

#if WITH_AUTOMATION_WORKER

namespace
{
	/**
	 * 用真实解析路径造一步，而不是手搓 `FGameTestStep`：
	 * 这样测到的就是"场景文件里写的那一行"能走到的状态，解析规则变了这里也跟着变。
	 * 解析失败时返回的 Step 是 reset 过的（动作名为 None），执行时会走"没有实现"的分支。
	 */
	FGameTestStep MakeStep(const FString& InText, int32 InIndex)
	{
		FString ParseError;
		const TSharedPtr<FJsonObject> Object = GameTestJson::ParseObjectFromText(InText, ParseError);

		FGameTestStep Step;
		TArray<FString> Errors;
		GameTestActions::ParseStep(Object, InIndex, Step, Errors);
		return Step;
	}

	/** 一定成功的步骤：`mark` 只读一个字符串，不碰世界也不碰 UMG。 */
	FGameTestStep MakeMarkStep(int32 InIndex)
	{
		return MakeStep(FString::Printf(TEXT(R"({"action":"mark","text":"step-%d"})"), InIndex), InIndex);
	}

	/**
	 * 一定失败的步骤：`mark` 缺 `text`。
	 *
	 * 选它当"失败"而不是 `click` 之类，是因为失败原因与引擎状态无关——无 PIE 的
	 * 自动化进程里也能稳定复现，测试不会因为跑在别人的机器上就换个结果。
	 */
	FGameTestStep MakeFailingStep(int32 InIndex, bool bContinueOnFail = false)
	{
		return MakeStep(FString::Printf(TEXT(R"({"action":"mark"%s})"),
			bContinueOnFail ? TEXT(R"(,"onFail":"continue")") : TEXT("")), InIndex);
	}

	/** 永远不满足的跨帧步骤（无 PIE 时 `pieReady` 恒假），用来测超时。 */
	FGameTestStep MakeNeverSatisfiedStep(int32 InIndex, int32 InTimeoutMs)
	{
		return MakeStep(FString::Printf(
			TEXT(R"({"action":"wait","until":"pieReady","timeoutMs":%d})"), InTimeoutMs), InIndex);
	}

	FGameTestRunRequest MakeRequest(TArray<FGameTestStep> InSteps)
	{
		FGameTestRunRequest Request;
		Request.Name = TEXT("runner-test");
		Request.RunId = TEXT("runner-test-1");
		Request.Revision = 7;
		Request.Steps = MoveTemp(InSteps);
		Request.StepDelayMs = 0;
		Request.DefaultStepTimeoutMs = 60000;
		return Request;
	}

	/** 报告里每一步的"序号=状态"，断言失败时能一眼看出实际跑成了什么。 */
	FString RunnerDescribeReport(const FGameTestReport& InReport)
	{
		TArray<FString> Parts;
		for (const FGameTestStepRecord& Record : InReport.Steps)
		{
			Parts.Add(FString::Printf(TEXT("#%d=%s"), Record.Step.Index,
				GameTestStatus::ToString(Record.Status)));
		}
		return FString::Printf(TEXT("共 %d 条 [%s]"), InReport.Steps.Num(), *FString::Join(Parts, TEXT(", ")));
	}

	/** 报告里某个序号出现了几条——用来抓"同一步被记录两次"。 */
	int32 CountByIndex(const FGameTestReport& InReport, int32 InIndex)
	{
		int32 Count = 0;
		for (const FGameTestStepRecord& Record : InReport.Steps)
		{
			if (Record.Step.Index == InIndex)
			{
				++Count;
			}
		}
		return Count;
	}

	/** 按序号取那一步的状态；不存在时返回 Skipped 并把 OutFound 置假。 */
	EGameTestStepStatus StatusOf(const FGameTestReport& InReport, int32 InIndex, bool& bOutFound)
	{
		for (const FGameTestStepRecord& Record : InReport.Steps)
		{
			if (Record.Step.Index == InIndex)
			{
				bOutFound = true;
				return Record.Status;
			}
		}
		bOutFound = false;
		return EGameTestStepStatus::Skipped;
	}

	FString MessageOf(const FGameTestReport& InReport, int32 InIndex)
	{
		for (const FGameTestStepRecord& Record : InReport.Steps)
		{
			if (Record.Step.Index == InIndex)
			{
				return Record.Message;
			}
		}
		return FString();
	}
}

TEST_CLASS_WITH_FLAGS(GameTestRunnerTest, "GameTest.Runner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/**
	 * 空队列必须在起跑前就被拒——否则会起一个"零步、立刻成功"的 run，
	 * 看起来像场景通过了，实际什么都没验。
	 */
	TEST_METHOD(StartRun_RejectsEmptyQueue)
	{
		FGameTestRunner Runner;
		FGameTestRuntimeState RuntimeState;
		FString Error;

		const bool bStarted = Runner.StartRun(MakeRequest({}), RuntimeState, nullptr, Error);

		ASSERT_THAT(IsFalse(bStarted, TEXT("空队列应拒绝")));
		ASSERT_THAT(IsFalse(Error.IsEmpty(), TEXT("拒绝时要给出原因")));
		ASSERT_THAT(IsFalse(Runner.IsBusy(), TEXT("被拒的 run 不该留下忙碌状态")));
	}

	/** 同一时刻只允许一个 run：第二个必须在起跑前被拒，而不是把第一个挤掉。 */
	TEST_METHOD(StartRun_RejectsWhenAlreadyRunning)
	{
		FGameTestRunner Runner;
		FGameTestRuntimeState RuntimeState;
		FString FirstError;
		ASSERT_THAT(IsTrue(Runner.StartRun(MakeRequest({ MakeMarkStep(0) }), RuntimeState, nullptr, FirstError),
			*FirstError));

		FString SecondError;
		const bool bSecond = Runner.StartRun(MakeRequest({ MakeMarkStep(0) }), RuntimeState, nullptr, SecondError);

		ASSERT_THAT(IsFalse(bSecond, TEXT("第二个 run 应被拒")));
		ASSERT_THAT(IsFalse(SecondError.IsEmpty(), TEXT("拒绝时要给出原因")));
		ASSERT_THAT(IsTrue(Runner.IsBusy(), TEXT("原 run 仍应在跑")));
	}

	/**
	 * 一帧内把能立刻做完的步骤连着跑完：编辑器被节流时一帧可能隔很久才来，
	 * 一步一帧会让整场景停摆。这条同时钉住"收尾在同一帧发生"——不能晚一帧。
	 */
	TEST_METHOD(Tick_RunsImmediateStepsInOneFrame)
	{
		FGameTestRunner Runner;
		FGameTestRuntimeState RuntimeState;
		FString Error;
		int32 FinishCount = 0;

		ASSERT_THAT(IsTrue(Runner.StartRun(
			MakeRequest({ MakeMarkStep(0), MakeMarkStep(1), MakeMarkStep(2) }),
			RuntimeState,
			[&FinishCount](const FGameTestReport&) { ++FinishCount; },
			Error), *Error));

		Runner.Tick(0.0);

		const FGameTestReport& Report = Runner.GetReport();
		ASSERT_THAT(IsTrue(Report.Steps.Num() == 3, RunnerDescribeReport(Report)));
		ASSERT_THAT(IsTrue(Report.CountByStatus(EGameTestStepStatus::Succeeded) == 3, RunnerDescribeReport(Report)));
		ASSERT_THAT(IsTrue(Report.bSucceeded, TEXT("三步全成功时报告应为成功")));
		ASSERT_THAT(IsTrue(FinishCount == 1, TEXT("收尾回调应恰好触发一次")));
		ASSERT_THAT(IsFalse(Runner.IsBusy(), TEXT("同帧就该收尾，不留忙碌状态")));
	}

	/**
	 * 步间隔只挡"下一步"，且只在成功后生效。
	 * 间隔内 Tick 不该推进任何东西——否则高亮一闪而过，人也看不清碰的是哪个控件。
	 */
	TEST_METHOD(Tick_HonoursStepDelayBetweenSteps)
	{
		FGameTestRunner Runner;
		FGameTestRuntimeState RuntimeState;
		FString Error;

		FGameTestRunRequest Request = MakeRequest({ MakeMarkStep(0), MakeMarkStep(1), MakeMarkStep(2) });
		Request.StepDelayMs = 1000;
		ASSERT_THAT(IsTrue(Runner.StartRun(Request, RuntimeState, nullptr, Error), *Error));

		Runner.Tick(0.0);
		ASSERT_THAT(IsTrue(Runner.GetReport().Steps.Num() == 1,
			TEXT("间隔 1000ms 时第一帧只该完成第 1 步")));

		Runner.Tick(0.5);
		ASSERT_THAT(IsTrue(Runner.GetReport().Steps.Num() == 1,
			TEXT("间隔未到不该推进")));

		Runner.Tick(1.0);
		ASSERT_THAT(IsTrue(Runner.GetReport().Steps.Num() == 2,
			TEXT("到点后该推进第 2 步")));

		Runner.Tick(2.0);
		const FGameTestReport& Report = Runner.GetReport();
		ASSERT_THAT(IsTrue(Report.Steps.Num() == 3, RunnerDescribeReport(Report)));
		ASSERT_THAT(IsTrue(Report.bSucceeded, RunnerDescribeReport(Report)));
	}

	/**
	 * 失败即停（默认 `onFail=stop`）：失败那步标 failed，**其余步骤如实标 skipped 而不是消失**。
	 * 步数对不上时，看报告的人会以为场景本来就是三步里跑两步。
	 */
	TEST_METHOD(Tick_FailsAndMarksRemainingSkipped)
	{
		FGameTestRunner Runner;
		FGameTestRuntimeState RuntimeState;
		FString Error;

		ASSERT_THAT(IsTrue(Runner.StartRun(
			MakeRequest({ MakeMarkStep(0), MakeFailingStep(1), MakeMarkStep(2) }),
			RuntimeState, nullptr, Error), *Error));

		Runner.Tick(0.0);

		const FGameTestReport& Report = Runner.GetReport();
		ASSERT_THAT(IsTrue(Report.Steps.Num() == 3, RunnerDescribeReport(Report)));

		bool bFound = false;
		ASSERT_THAT(IsTrue(StatusOf(Report, 0, bFound) == EGameTestStepStatus::Succeeded, RunnerDescribeReport(Report)));
		ASSERT_THAT(IsTrue(bFound, TEXT("第 1 步要在报告里")));
		ASSERT_THAT(IsTrue(StatusOf(Report, 1, bFound) == EGameTestStepStatus::Failed, RunnerDescribeReport(Report)));
		ASSERT_THAT(IsTrue(StatusOf(Report, 2, bFound) == EGameTestStepStatus::Skipped,
			FString::Printf(TEXT("未执行的步骤要标 skipped；%s"), *RunnerDescribeReport(Report))));

		ASSERT_THAT(IsFalse(Report.bSucceeded, TEXT("有失败步骤时报告不该算成功")));
		ASSERT_THAT(IsFalse(Runner.IsBusy(), TEXT("失败即停应同帧收尾")));
	}

	/** `onFail=continue`：失败那步记 failed，但后面照跑——用来一趟看多个断言的结果。 */
	TEST_METHOD(Tick_ContinuesAfterFailureWhenOnFailContinue)
	{
		FGameTestRunner Runner;
		FGameTestRuntimeState RuntimeState;
		FString Error;

		ASSERT_THAT(IsTrue(Runner.StartRun(
			MakeRequest({ MakeMarkStep(0), MakeFailingStep(1, /*bContinueOnFail=*/true), MakeMarkStep(2) }),
			RuntimeState, nullptr, Error), *Error));

		Runner.Tick(0.0);

		const FGameTestReport& Report = Runner.GetReport();
		ASSERT_THAT(IsTrue(Report.Steps.Num() == 3, RunnerDescribeReport(Report)));
		ASSERT_THAT(IsTrue(Report.CountByStatus(EGameTestStepStatus::Succeeded) == 2, RunnerDescribeReport(Report)));

		bool bFound = false;
		ASSERT_THAT(IsTrue(StatusOf(Report, 2, bFound) == EGameTestStepStatus::Succeeded,
			FString::Printf(TEXT("continue 时最后一步该跑完；%s"), *RunnerDescribeReport(Report))));
		ASSERT_THAT(IsFalse(Report.bSucceeded, TEXT("整次 run 有失败就不算成功")));
	}

	/**
	 * 跨帧步骤超时：标 failed 且消息里报出真正的上限——"没等到"必须能区分"等太短"和"等错了东西"。
	 */
	TEST_METHOD(Tick_TimesOutCrossFrameStep)
	{
		FGameTestRunner Runner;
		FGameTestRuntimeState RuntimeState;
		FString Error;

		ASSERT_THAT(IsTrue(Runner.StartRun(
			MakeRequest({ MakeNeverSatisfiedStep(0, 1000) }), RuntimeState, nullptr, Error), *Error));

		Runner.Tick(0.0);
		ASSERT_THAT(IsTrue(Runner.GetReport().Steps.Num() == 0,
			TEXT("跨帧步骤第一帧只是开始等，不该产生记录")));
		ASSERT_THAT(IsTrue(Runner.IsBusy(), TEXT("跨帧等待期间仍算忙碌")));

		Runner.Tick(1.0);

		const FGameTestReport& Report = Runner.GetReport();
		ASSERT_THAT(IsTrue(Report.Steps.Num() == 1, RunnerDescribeReport(Report)));

		bool bFound = false;
		ASSERT_THAT(IsTrue(StatusOf(Report, 0, bFound) == EGameTestStepStatus::Failed, RunnerDescribeReport(Report)));
		ASSERT_THAT(IsTrue(MessageOf(Report, 0).Contains(TEXT("超时")),
			FString::Printf(TEXT("超时消息要说清是超时；实际：%s"), *MessageOf(Report, 0))));
		ASSERT_THAT(IsFalse(Runner.IsBusy(), TEXT("超时后应收尾")));
	}

	/** 失败的步骤要叫一次留证据的回调（此时记录还没进报告，回调写进 Payload 的东西要进报告）；成功的不叫。 */
	TEST_METHOD(OnStepFailed_CalledOncePerFailedStepBeforeReport)
	{
		FGameTestRunner Runner;
		FGameTestRuntimeState RuntimeState;
		int32 Calls = 0;
		RuntimeState.OnStepFailed = [&Calls](FGameTestStepRecord& InOutRecord, const FString&)
		{
			++Calls;
			InOutRecord.Payload = MakeShared<FJsonObject>();
			InOutRecord.Payload->SetStringField(TEXT("evidence"), TEXT("x"));
		};
		FString Error;
		ASSERT_THAT(IsTrue(Runner.StartRun(
			MakeRequest({ MakeMarkStep(0), MakeFailingStep(1, /*bContinueOnFail=*/true), MakeMarkStep(2) }),
			RuntimeState, nullptr, Error), *Error));
		Runner.Tick(0.0);

		const FGameTestReport& Report = Runner.GetReport();
		ASSERT_THAT(IsTrue(Calls == 1, RunnerDescribeReport(Report)));
		ASSERT_THAT(IsTrue(Report.Steps[1].Payload.IsValid() && Report.Steps[1].Payload->HasField(TEXT("evidence")),
			TEXT("回调写进 Payload 的证据路径要进报告")));
	}

	/** 跨帧等待期间取消：当前步标 failed 且说明是取消，而不是伪装成超时。 */
	TEST_METHOD(Cancel_MarksInFlightStepFailedAndSkipsRest)
	{
		FGameTestRunner Runner;
		FGameTestRuntimeState RuntimeState;
		FString Error;

		ASSERT_THAT(IsTrue(Runner.StartRun(
			MakeRequest({ MakeNeverSatisfiedStep(0, 60000), MakeMarkStep(1) }),
			RuntimeState, nullptr, Error), *Error));

		Runner.Tick(0.0);
		Runner.Cancel(TEXT("测试取消"));

		const FGameTestReport& Report = Runner.GetReport();
		ASSERT_THAT(IsTrue(Report.Steps.Num() == 2, RunnerDescribeReport(Report)));

		bool bFound = false;
		ASSERT_THAT(IsTrue(StatusOf(Report, 0, bFound) == EGameTestStepStatus::Failed, RunnerDescribeReport(Report)));
		ASSERT_THAT(IsTrue(MessageOf(Report, 0).Contains(TEXT("取消")),
			FString::Printf(TEXT("被取消的那步要说明原因；实际：%s"), *MessageOf(Report, 0))));
		ASSERT_THAT(IsTrue(StatusOf(Report, 1, bFound) == EGameTestStepStatus::Skipped, RunnerDescribeReport(Report)));
		ASSERT_THAT(IsFalse(Runner.IsBusy(), TEXT("取消后应立刻收尾")));
	}

	/**
	 * 取消恰好落在**两步之间**（上一步已成功、下一步还没起步——步间隔期间就是这个窗口）。
	 *
	 * 这里没有"正在执行的步骤"可标失败：正确行为是把剩余步骤全部如实标 skipped。
	 * 若把它当成"当前步失败"去记，会用上一步的记录再 Add 一次——重复条目 + 篡改上一步状态
	 * + 中间那步凭空消失，报告步数与场景步数对不上。
	 */
	TEST_METHOD(Cancel_BetweenStepsSkipsRemainingWithoutDuplicating)
	{
		FGameTestRunner Runner;
		FGameTestRuntimeState RuntimeState;
		FString Error;

		FGameTestRunRequest Request = MakeRequest({ MakeMarkStep(0), MakeMarkStep(1), MakeMarkStep(2) });
		Request.StepDelayMs = 1000;
		ASSERT_THAT(IsTrue(Runner.StartRun(Request, RuntimeState, nullptr, Error), *Error));

		// 第一帧跑完第 1 步，随后进入步间隔：此刻既没有 in-flight 步骤，也还有剩余步骤。
		Runner.Tick(0.0);
		ASSERT_THAT(IsTrue(Runner.GetReport().Steps.Num() == 1, TEXT("前提：第一帧只完成第 1 步")));

		Runner.Cancel(TEXT("测试取消"));

		const FGameTestReport& Report = Runner.GetReport();
		ASSERT_THAT(IsTrue(Report.Steps.Num() == 3,
			FString::Printf(TEXT("报告步数要等于场景步数；%s"), *RunnerDescribeReport(Report))));
		ASSERT_THAT(IsTrue(CountByIndex(Report, 0) == 1,
			FString::Printf(TEXT("第 1 步不该被记录两次；%s"), *RunnerDescribeReport(Report))));
		ASSERT_THAT(IsTrue(CountByIndex(Report, 1) == 1,
			FString::Printf(TEXT("第 2 步不该消失；%s"), *RunnerDescribeReport(Report))));

		bool bFound = false;
		ASSERT_THAT(IsTrue(StatusOf(Report, 0, bFound) == EGameTestStepStatus::Succeeded,
			FString::Printf(TEXT("已完成的那步不该被取消改写；%s"), *RunnerDescribeReport(Report))));
		ASSERT_THAT(IsTrue(StatusOf(Report, 1, bFound) == EGameTestStepStatus::Skipped, RunnerDescribeReport(Report)));
		ASSERT_THAT(IsTrue(StatusOf(Report, 2, bFound) == EGameTestStepStatus::Skipped, RunnerDescribeReport(Report)));
	}

	/** 还没 Tick 就取消：全部步骤如实标 skipped（不能凭空多出一条空步骤记录）。 */
	TEST_METHOD(Cancel_BeforeAnyStepSkipsAll)
	{
		FGameTestRunner Runner;
		FGameTestRuntimeState RuntimeState;
		FString Error;

		ASSERT_THAT(IsTrue(Runner.StartRun(
			MakeRequest({ MakeMarkStep(0), MakeMarkStep(1) }), RuntimeState, nullptr, Error), *Error));

		Runner.Cancel(TEXT("测试取消"));

		const FGameTestReport& Report = Runner.GetReport();
		ASSERT_THAT(IsTrue(Report.Steps.Num() == 2,
			FString::Printf(TEXT("两条都该如实记 skipped；%s"), *RunnerDescribeReport(Report))));
		ASSERT_THAT(IsTrue(Report.CountByStatus(EGameTestStepStatus::Skipped) == 2, RunnerDescribeReport(Report)));
		ASSERT_THAT(IsTrue(MessageOf(Report, 0).IsEmpty() == false, TEXT("跳过原因要写出来")));
	}

	/** run 身份（runId / 场景名 / revision）从起跑那一刻就在报告里——`job` 轮询期间也要能读到。 */
	TEST_METHOD(StartRun_PutsRunIdentityInReport)
	{
		FGameTestRunner Runner;
		FGameTestRuntimeState RuntimeState;
		FString Error;

		ASSERT_THAT(IsTrue(Runner.StartRun(
			MakeRequest({ MakeMarkStep(0) }), RuntimeState, nullptr, Error), *Error));

		// 还没 Tick：进行中的报告就该带上身份（轮询方靠它认领这次 run）。
		const FGameTestReport& Running = Runner.GetReport();
		ASSERT_THAT(IsTrue(Running.RunId == TEXT("runner-test-1"), *Running.RunId));
		ASSERT_THAT(IsTrue(Running.Name == TEXT("runner-test"), *Running.Name));
		ASSERT_THAT(IsTrue(Running.Revision == 7, FString::Printf(TEXT("revision=%d"), Running.Revision)));

		Runner.Tick(0.0);
		ASSERT_THAT(IsTrue(Runner.GetReport().bSucceeded, TEXT("收尾后报告仍应可读")));
	}

	/** 收尾之后再 Tick 不该有任何副作用（心跳可能在收尾后又跑了一帧）。 */
	TEST_METHOD(Tick_AfterFinishIsNoOp)
	{
		FGameTestRunner Runner;
		FGameTestRuntimeState RuntimeState;
		FString Error;
		int32 FinishCount = 0;

		ASSERT_THAT(IsTrue(Runner.StartRun(
			MakeRequest({ MakeMarkStep(0) }),
			RuntimeState,
			[&FinishCount](const FGameTestReport&) { ++FinishCount; },
			Error), *Error));

		Runner.Tick(0.0);
		ASSERT_THAT(IsTrue(FinishCount == 1, TEXT("前提：第一帧就收尾")));

		Runner.Tick(100.0);

		ASSERT_THAT(IsTrue(Runner.GetReport().Steps.Num() == 1,
			TEXT("收尾后 Tick 不该再动报告")));
		ASSERT_THAT(IsTrue(FinishCount == 1, TEXT("收尾回调不该再触发")));
	}
};

#endif // WITH_AUTOMATION_WORKER
