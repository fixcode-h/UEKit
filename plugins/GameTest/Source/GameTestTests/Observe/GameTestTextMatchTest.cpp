// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Observe/GameTestTextMatch.h"

#if WITH_AUTOMATION_WORKER

using GameTestTextMatch::ETier;

TEST_CLASS_WITH_FLAGS(GameTestTextMatchTest, "GameTest.TextMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	TEST_METHOD(Compare_ExactWins)
	{
		ASSERT_THAT(IsTrue(GameTestTextMatch::Compare(TEXT("结束所有引导"), TEXT("结束所有引导")) == ETier::Exact));
	}

	TEST_METHOD(Compare_IgnoresNumbersTagsAndPunct)
	{
		ASSERT_THAT(IsTrue(GameTestTextMatch::Compare(TEXT("领取(3)"), TEXT("领取（0）")) == ETier::Normalized));
		ASSERT_THAT(IsTrue(GameTestTextMatch::Compare(TEXT("剩余 02:15"), TEXT("剩余 01:58")) == ETier::Normalized));
		ASSERT_THAT(IsTrue(GameTestTextMatch::Compare(TEXT("<red>Shop</>"), TEXT("shop")) == ETier::Normalized));
	}

	TEST_METHOD(Compare_ContainsNeedsEnoughText)
	{
		ASSERT_THAT(IsTrue(GameTestTextMatch::Compare(TEXT("商店"), TEXT("商店 NEW")) == ETier::Contains));
		// 一个字太短、短的不到长的 40% 都不算
		ASSERT_THAT(IsTrue(GameTestTextMatch::Compare(TEXT("确"), TEXT("确定")) == ETier::None));
		ASSERT_THAT(IsTrue(GameTestTextMatch::Compare(TEXT("战斗"), TEXT("开始挑战精英战斗副本")) == ETier::None));
	}

	TEST_METHOD(Compare_DifferentTextIsNone)
	{
		ASSERT_THAT(IsTrue(GameTestTextMatch::Compare(TEXT("结束所有引导"), TEXT("ResetPoint")) == ETier::None));
		ASSERT_THAT(IsTrue(GameTestTextMatch::Compare(TEXT(""), TEXT("ResetPoint")) == ETier::None));
	}

	TEST_METHOD(FilterBest_KeepsOnlyHighestTier)
	{
		TArray<int32> Hits;
		const TArray<FString> Texts = { TEXT("领取(3)"), TEXT("领取"), TEXT("关闭") };
		ASSERT_THAT(IsTrue(GameTestTextMatch::FilterBest(TEXT("领取"), Texts, Hits) == ETier::Exact));
		ASSERT_THAT(AreEqual(1, Hits.Num()));
		ASSERT_THAT(AreEqual(1, Hits[0]));
	}
}

#endif
