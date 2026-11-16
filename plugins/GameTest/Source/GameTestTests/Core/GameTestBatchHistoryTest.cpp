// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/GameTestBatch.h"
#include "Core/GameTestHistory.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_AUTOMATION_WORKER

namespace
{
	FGameTestReport MakeBatchReport(const FString& InRunId, bool bInSucceeded)
	{
		FGameTestReport Report;
		Report.RunId = InRunId;
		Report.Name = InRunId;
		Report.bSucceeded = bInSucceeded;
		if (!bInSucceeded)
		{
			FGameTestStepRecord Record;
			Record.Step.Index = 2;
			Record.Status = EGameTestStepStatus::Failed;
			Record.Message = TEXT("没找到按钮");
			Report.Steps.Add(Record);
		}
		return Report;
	}
}

TEST_CLASS_WITH_FLAGS(GameTestBatchTest, "GameTest.Batch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 一个接一个：失败的、没能开始的都不挡后面的，全部有结论才结束。 */
	TEST_METHOD(RunsEachScenarioAndFinishesWhenAllDone)
	{
		FGameTestBatch Batch;
		Batch.Start(TEXT("batch-1"), TEXT("tag:smoke"), { TEXT("a.json"), TEXT("b.json"), TEXT("c.json") });
		ASSERT_THAT(IsTrue(Batch.IsActive()));

		ASSERT_THAT(AreEqual(0, Batch.TakeNext()));
		// 起的那一下就跑完了（第一步失败）：还没记 runId 也要记到正在跑的那项上
		Batch.OnRunFinished(MakeBatchReport(TEXT("a-1"), false));
		Batch.MarkStarted(0, TEXT("a-1"), TEXT("a"));
		ASSERT_THAT(IsTrue(Batch.GetItem(0).Status == TEXT("failed")));
		ASSERT_THAT(AreEqual(3, Batch.GetItem(0).FailedStep));

		ASSERT_THAT(AreEqual(1, Batch.TakeNext()));
		Batch.MarkError(1, TEXT("解析失败"));

		ASSERT_THAT(AreEqual(2, Batch.TakeNext()));
		ASSERT_THAT(IsFalse(Batch.FinishIfDone(), TEXT("还有一项在跑，不能结束")));
		Batch.OnRunFinished(MakeBatchReport(TEXT("c-3"), true));

		ASSERT_THAT(AreEqual(INDEX_NONE, Batch.TakeNext()));
		ASSERT_THAT(IsTrue(Batch.FinishIfDone()));
		ASSERT_THAT(IsFalse(Batch.IsActive()));
		ASSERT_THAT(AreEqual(1, Batch.Count(TEXT("passed"))));
		ASSERT_THAT(AreEqual(1, Batch.Count(TEXT("error"))));
	}

	TEST_METHOD(CancelSkipsTheRest)
	{
		FGameTestBatch Batch;
		Batch.Start(TEXT("batch-2"), TEXT("x"), { TEXT("a.json"), TEXT("b.json") });
		Batch.TakeNext();
		Batch.Cancel();
		Batch.OnRunFinished(MakeBatchReport(TEXT("a-1"), false));
		ASSERT_THAT(IsFalse(Batch.IsActive()));
		ASSERT_THAT(IsTrue(Batch.GetItem(0).Status == TEXT("failed"), TEXT("正在跑的那项由执行器标失败")));
		ASSERT_THAT(IsTrue(Batch.GetItem(1).Status == TEXT("skipped")));
	}
};

TEST_CLASS_WITH_FLAGS(GameTestHistoryTest, "GameTest.History",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 最新在前、只留 10 次；压缩串里 p 通过 f 失败。 */
	TEST_METHOD(AppendKeepsNewestTen)
	{
		FJsonObject All;
		for (int32 Index = 0; Index < 12; ++Index)
		{
			GameTestHistory::Append(All, TEXT("smoke/a.json"),
				GameTestHistory::MakeEntry(MakeBatchReport(FString::Printf(TEXT("a-%d"), Index), Index != 11), TEXT("Saved/GameTest/x")));
		}

		const TSharedPtr<FJsonObject> Scenarios = GameTestJson::TryGetObject(All, TEXT("scenarios"));
		const TArray<TSharedPtr<FJsonValue>>* List = Scenarios.IsValid() ? GameTestJson::TryGetArray(*Scenarios, TEXT("smoke/a.json")) : nullptr;
		ASSERT_THAT(IsTrue(List != nullptr && List->Num() == GameTestHistory::Keep));

		FString First;
		GameTestJson::TryGetString(*(*List)[0]->AsObject(), TEXT("runId"), First);
		ASSERT_THAT(IsTrue(First == TEXT("a-11"), *First));

		FString Marks;
		GameTestJson::TryGetString(*GameTestHistory::Compact(All), TEXT("smoke/a.json"), Marks);
		ASSERT_THAT(IsTrue(Marks == TEXT("fppppppppp"), *Marks));
	}
};

#endif
