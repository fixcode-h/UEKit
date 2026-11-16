// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Shot/GameTestShot.h"

#include "Dom/JsonObject.h"
#include "Misc/Paths.h"

#if WITH_AUTOMATION_WORKER

/**
 * 截图的调用方可能只关心成没成，结果对象传的是空指针（失败留证据就是这样）：
 * 1.2.0-beta.2 在这条路上解引用空指针，测试没跑完就停止时编辑器直接崩。这里钉住「空指针进来不崩、出去是个真对象」。
 */
TEST_CLASS_WITH_FLAGS(GameTestShotTest, "GameTest.Shot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	TEST_METHOD(CaptureGameWindow_NullPayloadWithoutWorld_FailsWithoutCrash)
	{
		TSharedPtr<FJsonObject> Payload;
		FString Message;
		const FString Path = FPaths::Combine(GameTestShot::GetOutputRootDir(), TEXT("shot-test"), TEXT("never.png"));

		const bool bOk = GameTestShot::CaptureGameWindow(nullptr, Path, nullptr, Message, Payload);

		ASSERT_THAT(IsFalse(bOk, TEXT("没有世界时截图应当失败")));
		ASSERT_THAT(IsTrue(Payload.IsValid(), TEXT("结果对象应当被补成真对象，调用方往里写字段不会崩")));
		ASSERT_THAT(IsFalse(Message.IsEmpty(), TEXT("失败要写明原因")));
	}
};

#endif	// WITH_AUTOMATION_WORKER
