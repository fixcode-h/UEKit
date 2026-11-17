// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/GameTestAction.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestRunConfig.h"
#include "Core/GameTestTrajectory.h"
#include "Core/GameTestTypes.h"

#include "Dom/JsonObject.h"

#if WITH_AUTOMATION_WORKER

TEST_CLASS_WITH_FLAGS(GameTestTrajectoryTest, "GameTest.Trajectory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	TEST_METHOD(Simplify_StraightLineKeepsEnds)
	{
		TArray<FVector> Points;
		for (int32 Index = 0; Index <= 100; ++Index)
		{
			Points.Add(FVector(Index * 50.0, 0.0, 88.0));
		}
		const TArray<int32> Kept = GameTestTrajectory::Simplify(Points, 30.0, {});
		ASSERT_THAT(AreEqual(2, Kept.Num()));
		ASSERT_THAT(AreEqual(0, Kept[0]));
		ASSERT_THAT(AreEqual(100, Kept[1]));
	}

	TEST_METHOD(Simplify_KeepsCornerAndForcedPoints)
	{
		// L 形：拐角一定留；直线中间的起跳点也要留。
		TArray<FVector> Points = {FVector(0, 0, 0), FVector(500, 0, 0), FVector(1000, 0, 0), FVector(1000, 500, 0), FVector(1000, 1000, 0)};
		const TArray<int32> Kept = GameTestTrajectory::Simplify(Points, 30.0, {1});
		ASSERT_THAT(IsTrue(Kept.Contains(1), TEXT("强制保留的点")));
		ASSERT_THAT(IsTrue(Kept.Contains(2), TEXT("拐角")));
		ASSERT_THAT(IsFalse(Kept.Contains(3)));
	}

	TEST_METHOD(SimplifyToLimit_RespectsMax)
	{
		TArray<FVector> Points;
		for (int32 Index = 0; Index < 2000; ++Index)
		{
			Points.Add(FVector(Index * 20.0, FMath::Sin(Index * 0.3) * 200.0, 0.0));
		}
		const TArray<int32> Kept = GameTestTrajectory::SimplifyToLimit(Points, 30.0, 300, {});
		ASSERT_THAT(IsTrue(Kept.Num() <= 300, FString::FromInt(Kept.Num())));
		ASSERT_THAT(AreEqual(0, Kept[0]));
		ASSERT_THAT(AreEqual(1999, Kept.Last()));
	}

	TEST_METHOD(FindJoinIndex_ContinuesForward)
	{
		const TArray<FVector> Path = {FVector(0, 0, 0), FVector(1000, 0, 0), FVector(1000, 1000, 0)};
		double Distance = 0.0;
		FVector Projected;
		// 站在第一段中间、偏开 100：接上第一段，往它的终点（下标 1）走。
		ASSERT_THAT(AreEqual(1, GameTestTrajectory::FindJoinIndex(Path, FVector(500, 100, 0), Distance, Projected)));
		ASSERT_THAT(IsTrue(FMath::IsNearlyEqual(Distance, 100.0, 0.01)));
		// 站在第二段旁边：往下标 2 走。
		ASSERT_THAT(AreEqual(2, GameTestTrajectory::FindJoinIndex(Path, FVector(1200, 600, 0), Distance, Projected)));
	}

	TEST_METHOD(PathJson_RoundTrips)
	{
		const TArray<FVector> Path = {FVector(1.25, -2.0, 3.0), FVector(100.0, 200.0, 300.0)};
		TArray<FVector> Read;
		FString Error;
		ASSERT_THAT(IsTrue(GameTestTrajectory::PathFromJson(GameTestTrajectory::PathToJson(Path), Read, Error), Error));
		ASSERT_THAT(AreEqual(2, Read.Num()));
		ASSERT_THAT(IsTrue(FMath::IsNearlyEqual(Read[0].X, 1.3, 0.001) || FMath::IsNearlyEqual(Read[0].X, 1.2, 0.001)));
	}

	TEST_METHOD(ParseStep_MoveAcceptsRecordedFields)
	{
		FString Error;
		const TSharedPtr<FJsonObject> Json = GameTestJson::ParseObjectFromText(TEXT(R"({"action":"move","to":[1,2,3],"yaw":90,"pitch":-10,
			"via":"auto","path":[[0,0,0],[1,2,3]],"jumps":[0],"hint":{"durationMs":1000,"nav":false}})"), Error);
		FGameTestStep Step;
		TArray<FString> Errors;
		ASSERT_THAT(IsTrue(GameTestActions::ParseStep(Json, 0, Step, Errors), FString::Join(Errors, TEXT("；"))));
	}

	TEST_METHOD(ParseStep_MoveRejectsTypo)
	{
		FString Error;
		const TSharedPtr<FJsonObject> Json = GameTestJson::ParseObjectFromText(TEXT(R"({"action":"move","to":[1,2,3],"radus":80})"), Error);
		FGameTestStep Step;
		TArray<FString> Errors;
		ASSERT_THAT(IsFalse(GameTestActions::ParseStep(Json, 0, Step, Errors)));
	}

	TEST_METHOD(RunConfig_MoveSectionValidated)
	{
		FGameTestRunConfig Config;
		FString Error;
		const TSharedPtr<FJsonObject> Good = GameTestJson::ParseObjectFromText(
			TEXT(R"({"move":{"fallback":"teleport","teleportCommand":"gm.Goto {x} {y} {z}","hotkeys":false}})"), Error);
		ASSERT_THAT(IsTrue(Good.IsValid() && GameTestRunConfig::FromJson(*Good, Config, Error), Error));
		ASSERT_THAT(IsTrue(Config.Move.IsValid()));

		const TSharedPtr<FJsonObject> Bad = GameTestJson::ParseObjectFromText(TEXT(R"({"move":{"fallback":"fly"}})"), Error);
		ASSERT_THAT(IsFalse(Bad.IsValid() && GameTestRunConfig::FromJson(*Bad, Config, Error)));
		const TSharedPtr<FJsonObject> Unknown = GameTestJson::ParseObjectFromText(TEXT(R"({"move":{"hotkey":true}})"), Error);
		ASSERT_THAT(IsFalse(Unknown.IsValid() && GameTestRunConfig::FromJson(*Unknown, Config, Error)));
	}
};

#endif
