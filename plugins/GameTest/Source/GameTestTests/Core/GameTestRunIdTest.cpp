// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/GameTestTypes.h"

#if WITH_AUTOMATION_WORKER

/**
 * `runId` 要直接进 `gt.runs/<runId>` 的路径段，所以这里钉住两件事：
 * 结果只含 `[a-z0-9-]`，且不同名字不会折成同一个 id。
 */
TEST_CLASS_WITH_FLAGS(GameTestRunIdTest, "GameTest.RunId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	TEST_METHOD(MakeRunId_KeepsPlainNamesReadable)
	{
		ASSERT_THAT(IsTrue(GameTestRun::MakeRunId(TEXT("showcase-smoke"), 3) == TEXT("showcase-smoke-3"),
			*GameTestRun::MakeRunId(TEXT("showcase-smoke"), 3)));
	}

	TEST_METHOD(MakeRunId_FoldsNonAsciiAndPunctuation)
	{
		// 场景名可以是中文：折成 slug，中文一个都不留，但序号保证仍唯一。
		const FString Chinese = GameTestRun::MakeRunId(TEXT("载具开火"), 7);
		ASSERT_THAT(IsTrue(Chinese == TEXT("run-7"), *Chinese));

		const FString Mixed = GameTestRun::MakeRunId(TEXT("Player Fire!"), 1);
		ASSERT_THAT(IsTrue(Mixed == TEXT("player-fire-1"), *Mixed));

		const FString Spaced = GameTestRun::MakeRunId(TEXT("  a  b  "), 2);
		ASSERT_THAT(IsTrue(Spaced == TEXT("a-b-2"), *Spaced));
	}

	TEST_METHOD(MakeRunId_NeverProducesEmptySlug)
	{
		// 名字全被折掉时退回 run，否则会产出 "-3" 这种以连字符开头的 id。
		const FString OnlyPunctuation = GameTestRun::MakeRunId(TEXT("!!!"), 3);
		ASSERT_THAT(IsTrue(OnlyPunctuation == TEXT("run-3"), *OnlyPunctuation));
		ASSERT_THAT(IsTrue(GameTestRun::MakeRunId(TEXT(""), 1) == TEXT("run-1")));
	}

	TEST_METHOD(MakeRunId_ResultIsUrlSafe)
	{
		const FString Id = GameTestRun::MakeRunId(TEXT("场景 A/B:c?d#e"), 12);
		for (const TCHAR Character : Id)
		{
			const bool bAllowed = (Character >= TEXT('a') && Character <= TEXT('z'))
				|| (Character >= TEXT('0') && Character <= TEXT('9'))
				|| Character == TEXT('-');
			ASSERT_THAT(IsTrue(bAllowed, FString::Printf(TEXT("'%s' 里有 URL 不安全字符"), *Id)));
		}
	}
};

#endif	// WITH_AUTOMATION_WORKER
