// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/GameTestJson.h"
#include "Core/GameTestRunConfig.h"

#include "Dom/JsonObject.h"

#if WITH_AUTOMATION_WORKER

namespace
{
	bool ParseRunConfig(const TCHAR* InText, FGameTestRunConfig& OutConfig, FString& OutError)
	{
		const TSharedPtr<FJsonObject> Json = GameTestJson::ParseObjectFromText(InText, OutError);
		return Json.IsValid() && GameTestRunConfig::FromJson(*Json, OutConfig, OutError);
	}
}

TEST_CLASS_WITH_FLAGS(GameTestRunConfigTest, "GameTest.RunConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	TEST_METHOD(Parse_ReadsAllActions)
	{
		FGameTestRunConfig Config;
		FString Error;
		ASSERT_THAT(IsTrue(ParseRunConfig(TEXT(R"({"uiWaitTimeoutMs":0,"blockers":[
			{"widget":"WBP_Notice"},
			{"widget":"WBP_Guide_C","action":"call","function":"Skip","delayMs":300},
			{"widget":"/Game/UI/WBP_Reconnect","action":"command","command":"gm.Reconnect","enabled":false}]})"), Config, Error), Error));
		ASSERT_THAT(AreEqual(0, Config.UiWaitTimeoutMs));
		ASSERT_THAT(AreEqual(3, Config.Blockers.Num()));
		ASSERT_THAT(IsTrue(Config.Blockers[0].Action == EGameTestBlockerAction::Close, TEXT("没写 action 默认 close")));
		ASSERT_THAT(IsTrue(Config.Blockers[1].Action == EGameTestBlockerAction::Call));
		ASSERT_THAT(AreEqual(300, Config.Blockers[1].DelayMs));
		ASSERT_THAT(IsFalse(Config.Blockers[2].bEnabled));
	}

	TEST_METHOD(Parse_MissingWaitMeansUseSettings)
	{
		FGameTestRunConfig Config;
		FString Error;
		ASSERT_THAT(IsTrue(ParseRunConfig(TEXT(R"({"blockers":[]})"), Config, Error), Error));
		ASSERT_THAT(AreEqual(-1, Config.UiWaitTimeoutMs));
	}

	TEST_METHOD(Parse_RejectsIncompleteRules)
	{
		FGameTestRunConfig Config;
		FString Error;
		ASSERT_THAT(IsFalse(ParseRunConfig(TEXT(R"({"blockers":[{"widget":"WBP_A","action":"call"}]})"), Config, Error)));
		ASSERT_THAT(IsTrue(Error.Contains(TEXT("function")), Error));
		ASSERT_THAT(IsFalse(ParseRunConfig(TEXT(R"({"blockers":[{"action":"close"}]})"), Config, Error)));
		ASSERT_THAT(IsFalse(ParseRunConfig(TEXT(R"({"blockers":[{"widget":"WBP_A","action":"hide"}]})"), Config, Error)));
		ASSERT_THAT(IsFalse(ParseRunConfig(TEXT(R"({"uiWaitTimeoutMs":-5})"), Config, Error)));
	}

	TEST_METHOD(ToJson_RoundTrips)
	{
		FGameTestRunConfig Config;
		FString Error;
		ASSERT_THAT(IsTrue(ParseRunConfig(TEXT(R"({"uiWaitTimeoutMs":15000,"blockers":[{"widget":"WBP_Guide","action":"call","function":"Skip","note":"新手引导"}]})"), Config, Error), Error));
		FGameTestRunConfig Again;
		ASSERT_THAT(IsTrue(GameTestRunConfig::FromJson(*GameTestRunConfig::ToJson(Config), Again, Error), Error));
		ASSERT_THAT(AreEqual(15000, Again.UiWaitTimeoutMs));
		ASSERT_THAT(AreEqual(FString(TEXT("Skip")), Again.Blockers[0].Function));
		ASSERT_THAT(AreEqual(FString(TEXT("新手引导")), Again.Blockers[0].Note));
	}
};

#endif
