// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/GameTestJson.h"
#include "Record/GameTestRecordShot.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_AUTOMATION_WORKER

namespace
{
	bool ParseShots(const TCHAR* InText, FGameTestShotOptions& OutOptions, FString& OutError)
	{
		const TSharedPtr<FJsonObject> Json = GameTestJson::ParseObjectFromText(InText, OutError);
		return Json.IsValid() && FGameTestShotOptions::FromJson(Json->TryGetField(TEXT("shots")), OutOptions, OutError);
	}

	FGameTestShotOptions Expanded()
	{
		FGameTestShotOptions Options;
		Options.bExpand = true;
		return Options;
	}
}

TEST_CLASS_WITH_FLAGS(GameTestRecordShotTest, "GameTest.RecordShot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 默认不外扩：截的就是控件那一块。 */
	TEST_METHOD(Crop_DefaultIsTargetOnly)
	{
		const FGameTestShotOptions Options;
		ASSERT_THAT(IsFalse(Options.bExpand));
		const FIntRect Crop = FGameTestRecordShots::ComputeCrop(FIntRect(500, 400, 700, 460), FIntPoint(1920, 1080), Options);
		ASSERT_THAT(AreEqual(500, Crop.Min.X));
		ASSERT_THAT(AreEqual(700, Crop.Max.X));
		ASSERT_THAT(AreEqual(400, Crop.Min.Y));
		ASSERT_THAT(AreEqual(460, Crop.Max.Y));
		// 一部分在屏幕外：只留看得见的
		const FIntRect Edge = FGameTestRecordShots::ComputeCrop(FIntRect(-20, 10, 40, 30), FIntPoint(1280, 720), Options);
		ASSERT_THAT(AreEqual(0, Edge.Min.X));
		ASSERT_THAT(AreEqual(40, Edge.Max.X));
	}

	/** 开了外扩：左右按宽 20%、上下按高 50%，各自封顶（默认 80 / 40）。 */
	TEST_METHOD(Crop_PadsEachAxisByItsOwnLength)
	{
		const FGameTestShotOptions Options = Expanded();
		const FIntRect Crop = FGameTestRecordShots::ComputeCrop(FIntRect(500, 400, 700, 460), FIntPoint(1920, 1080), Options);
		ASSERT_THAT(AreEqual(460, Crop.Min.X));
		ASSERT_THAT(AreEqual(740, Crop.Max.X));
		ASSERT_THAT(AreEqual(370, Crop.Min.Y));
		ASSERT_THAT(AreEqual(490, Crop.Max.Y));
	}

	/** 细长条：上下只扩高的一半（不再被最小高度撑开），左右到上限为止。 */
	TEST_METHOD(Crop_ThinStripKeepsRatio)
	{
		const FGameTestShotOptions Options = Expanded();
		const FIntRect Strip = FGameTestRecordShots::ComputeCrop(FIntRect(300, 500, 1500, 520), FIntPoint(1920, 1080), Options);
		ASSERT_THAT(AreEqual(220, Strip.Min.X));
		ASSERT_THAT(AreEqual(1580, Strip.Max.X));
		ASSERT_THAT(AreEqual(490, Strip.Min.Y));
		ASSERT_THAT(AreEqual(530, Strip.Max.Y));
	}

	/** 贴着屏幕边的按钮：外扩后的框往里挪，不出图。 */
	TEST_METHOD(Crop_ShiftsInsideImage)
	{
		const FGameTestShotOptions Options = Expanded();
		const FIntRect Crop = FGameTestRecordShots::ComputeCrop(FIntRect(0, 0, 40, 30), FIntPoint(1280, 720), Options);
		ASSERT_THAT(AreEqual(0, Crop.Min.X));
		ASSERT_THAT(AreEqual(0, Crop.Min.Y));
		ASSERT_THAT(AreEqual(56, Crop.Width()));
		ASSERT_THAT(AreEqual(60, Crop.Height()));
	}

	TEST_METHOD(FitInside_KeepsAspect)
	{
		const FIntPoint Fit = FGameTestRecordShots::FitInside(FIntPoint(1280, 400), FIntPoint(640, 360));
		ASSERT_THAT(AreEqual(640, Fit.X));
		ASSERT_THAT(AreEqual(200, Fit.Y));
		const FIntPoint Same = FGameTestRecordShots::FitInside(FIntPoint(300, 100), FIntPoint(640, 360));
		ASSERT_THAT(AreEqual(300, Same.X));
	}

	TEST_METHOD(Options_ParseAndReject)
	{
		FGameTestShotOptions Options;
		FString Error;
		ASSERT_THAT(IsTrue(ParseShots(TEXT(R"({"shots":false})"), Options, Error), Error));
		ASSERT_THAT(IsFalse(Options.bEnabled));
		ASSERT_THAT(IsTrue(ParseShots(TEXT(R"({"shots":{"expand":true,"padX":0.1,"padXMax":50,"padY":1,"padYMax":30,"maxWidth":800}})"), Options, Error), Error));
		ASSERT_THAT(IsTrue(Options.bEnabled));
		ASSERT_THAT(IsTrue(Options.bExpand));
		ASSERT_THAT(AreEqual(50, Options.PadXMax));
		ASSERT_THAT(AreEqual(30, Options.PadYMax));
		ASSERT_THAT(AreEqual(800, Options.MaxWidth));
		// 旧面板的写法：一个比例管四边，照旧外扩
		ASSERT_THAT(IsTrue(ParseShots(TEXT(R"({"shots":{"padRatio":0.3,"padMax":100}})"), Options, Error), Error));
		ASSERT_THAT(IsTrue(Options.bExpand));
		ASSERT_THAT(AreEqual(100, Options.PadXMax));
		ASSERT_THAT(AreEqual(100, Options.PadYMax));
		ASSERT_THAT(IsTrue(ParseShots(TEXT(R"({"shots":{"maxWidth":800}})"), Options, Error), Error));
		ASSERT_THAT(IsFalse(Options.bExpand, TEXT("没写 expand 默认不外扩")));
		ASSERT_THAT(IsTrue(ParseShots(TEXT(R"({})"), Options, Error), Error));
		ASSERT_THAT(IsTrue(Options.bEnabled, TEXT("没写 shots 默认开")));
		ASSERT_THAT(IsFalse(ParseShots(TEXT(R"({"shots":{"padding":10}})"), Options, Error)));
		ASSERT_THAT(IsFalse(ParseShots(TEXT(R"({"shots":{"maxWidth":10}})"), Options, Error)));
		ASSERT_THAT(IsFalse(ParseShots(TEXT(R"({"shots":"yes"})"), Options, Error)));
	}
};

#endif
