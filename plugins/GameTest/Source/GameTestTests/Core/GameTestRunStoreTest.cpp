// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/GameTestJson.h"
#include "Core/GameTestRunStore.h"
#include "Core/GameTestTypes.h"

#if WITH_AUTOMATION_WORKER

namespace
{
	/** 造一份最小报告：只要 runId 与摘要那几个字段，够验环形与摘要契约。 */
	TSharedPtr<FJsonObject> MakeReport(const FString& InRunId)
	{
		FString Error;
		const TSharedPtr<FJsonObject> Report = GameTestJson::ParseObjectFromText(
			FString::Printf(TEXT(R"({"runId":"%s","name":"scene","revision":3,"succeeded":true,"startedAtUtc":"t0","finishedAtUtc":"t1","durationMs":120.0,"total":4,"failed":0})"),
				*InRunId),
			Error);
		return Report;
	}
}

TEST_CLASS_WITH_FLAGS(GameTestRunStoreTest, "GameTest.RunStore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	TEST_METHOD(AddAndFind_KeepsReportRetrievable)
	{
		GameTestRunStore Store(5);
		Store.Add(MakeReport(TEXT("a-1")));

		ASSERT_THAT(AreEqual(1, Store.Num()));
		ASSERT_THAT(IsTrue(Store.Find(TEXT("a-1")).IsValid(), TEXT("刚加进去的要能查到")));
		ASSERT_THAT(IsFalse(Store.Find(TEXT("missing")).IsValid(), TEXT("不存在的 runId 返回空，而不是别的报告")));
	}

	TEST_METHOD(Add_EvictsOldestWhenOverCapacity)
	{
		GameTestRunStore Store(3);
		Store.Add(MakeReport(TEXT("r-1")));
		Store.Add(MakeReport(TEXT("r-2")));
		Store.Add(MakeReport(TEXT("r-3")));
		Store.Add(MakeReport(TEXT("r-4")));

		ASSERT_THAT(AreEqual(3, Store.Num(), TEXT("上限是硬上限")));
		ASSERT_THAT(IsFalse(Store.Find(TEXT("r-1")).IsValid(), TEXT("最旧的被淘汰")));
		ASSERT_THAT(IsTrue(Store.Find(TEXT("r-2")).IsValid(), TEXT("次旧的还在")));
		ASSERT_THAT(IsTrue(Store.Find(TEXT("r-4")).IsValid(), TEXT("最新的在")));
	}

	TEST_METHOD(ListSummaries_PutsNewestFirst)
	{
		GameTestRunStore Store(5);
		Store.Add(MakeReport(TEXT("r-1")));
		Store.Add(MakeReport(TEXT("r-2")));

		const TArray<TSharedPtr<FJsonObject>> Summaries = Store.ListSummaries();
		ASSERT_THAT(AreEqual(2, Summaries.Num()));

		FString First;
		GameTestJson::TryGetString(*Summaries[0], TEXT("runId"), First);
		ASSERT_THAT(IsTrue(First == TEXT("r-2"), *First));
	}

	TEST_METHOD(MakeSummary_CarriesTheFieldsTheListNeeds)
	{
		const TSharedPtr<FJsonObject> Summary = GameTestRunStore::MakeSummary(*MakeReport(TEXT("a-1")));

		FString RunId;
		ASSERT_THAT(IsTrue(GameTestJson::TryGetString(*Summary, TEXT("runId"), RunId) && RunId == TEXT("a-1"), TEXT("摘要要带 runId")));

		double Revision = 0.0;
		ASSERT_THAT(IsTrue(GameTestJson::TryGetNumber(*Summary, TEXT("revision"), Revision) && Revision == 3.0, TEXT("摘要要带 revision")));

		double Duration = 0.0;
		ASSERT_THAT(IsTrue(GameTestJson::TryGetNumber(*Summary, TEXT("durationMs"), Duration) && Duration == 120.0, TEXT("摘要要带耗时")));

		bool bSucceeded = false;
		ASSERT_THAT(IsTrue(GameTestJson::TryGetBool(*Summary, TEXT("succeeded"), bSucceeded) && bSucceeded, TEXT("摘要要带成败")));

		// 摘要是给清单看的，不该把逐步记录整份搬进来。
		ASSERT_THAT(IsFalse(Summary->HasField(TEXT("steps")), TEXT("摘要不含 steps")));
	}

	TEST_METHOD(SetCapacity_ClampsToAtLeastOne)
	{
		GameTestRunStore Store(0);
		Store.Add(MakeReport(TEXT("r-1")));
		Store.Add(MakeReport(TEXT("r-2")));

		// 上限 0 没有任何意义（历史永远是空的），按 1 处理比静默失效好。
		ASSERT_THAT(AreEqual(1, Store.Num()));
		ASSERT_THAT(IsTrue(Store.Find(TEXT("r-2")).IsValid(), TEXT("留下的是最新的那份")));
	}

	TEST_METHOD(Add_IgnoresInvalidJson)
	{
		GameTestRunStore Store(3);
		Store.Add(nullptr);
		ASSERT_THAT(AreEqual(0, Store.Num(), TEXT("空报告不占位")));
	}
};

#endif	// WITH_AUTOMATION_WORKER
