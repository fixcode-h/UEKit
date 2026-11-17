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
}

TEST_CLASS_WITH_FLAGS(GameTestRecordShotTest, "GameTest.RecordShot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 中等大小的按钮：每边外扩宽高的一半，不超上限。 */
	TEST_METHOD(Crop_PadsByRatio)
	{
		const FGameTestShotOptions Options;
		const FIntRect Crop = FGameTestRecordShots::ComputeCrop(FIntRect(500, 400, 700, 460), FIntPoint(1920, 1080), Options);
		ASSERT_THAT(AreEqual(400, Crop.Min.X));
		ASSERT_THAT(AreEqual(800, Crop.Max.X));
		// 高 60 的一半是 30（≥ 最少 24）；外扩后 120 正好是最小高度
		ASSERT_THAT(AreEqual(370, Crop.Min.Y));
		ASSERT_THAT(AreEqual(490, Crop.Max.Y));
	}

	/** 小图标补到最小尺寸，大控件外扩到上限 160 为止。 */
	TEST_METHOD(Crop_MinSizeAndPadLimit)
	{
		const FGameTestShotOptions Options;
		const FIntRect Small = FGameTestRecordShots::ComputeCrop(FIntRect(100, 100, 120, 120), FIntPoint(1920, 1080), Options);
		ASSERT_THAT(AreEqual(200, Small.Width()));
		ASSERT_THAT(AreEqual(120, Small.Height()));
		const FIntRect Big = FGameTestRecordShots::ComputeCrop(FIntRect(400, 300, 1400, 800), FIntPoint(1920, 1080), Options);
		ASSERT_THAT(AreEqual(240, Big.Min.X));
		ASSERT_THAT(AreEqual(1560, Big.Max.X));
	}

	/** 贴着屏幕边的按钮：框往里挪，不出图。 */
	TEST_METHOD(Crop_ShiftsInsideImage)
	{
		const FGameTestShotOptions Options;
		const FIntRect Crop = FGameTestRecordShots::ComputeCrop(FIntRect(0, 0, 40, 30), FIntPoint(1280, 720), Options);
		ASSERT_THAT(AreEqual(0, Crop.Min.X));
		ASSERT_THAT(AreEqual(0, Crop.Min.Y));
		ASSERT_THAT(AreEqual(200, Crop.Width()));
		ASSERT_THAT(AreEqual(120, Crop.Height()));
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
		ASSERT_THAT(IsTrue(ParseShots(TEXT(R"({"shots":{"padRatio":0.3,"padMax":100,"maxWidth":800}})"), Options, Error), Error));
		ASSERT_THAT(IsTrue(Options.bEnabled));
		ASSERT_THAT(AreEqual(100, Options.PadMax));
		ASSERT_THAT(AreEqual(800, Options.MaxWidth));
		ASSERT_THAT(IsTrue(ParseShots(TEXT(R"({})"), Options, Error), Error));
		ASSERT_THAT(IsTrue(Options.bEnabled, TEXT("没写 shots 默认开")));
		ASSERT_THAT(IsFalse(ParseShots(TEXT(R"({"shots":{"padding":10}})"), Options, Error)));
		ASSERT_THAT(IsFalse(ParseShots(TEXT(R"({"shots":{"maxWidth":10}})"), Options, Error)));
		ASSERT_THAT(IsFalse(ParseShots(TEXT(R"({"shots":"yes"})"), Options, Error)));
	}
};

#endif
