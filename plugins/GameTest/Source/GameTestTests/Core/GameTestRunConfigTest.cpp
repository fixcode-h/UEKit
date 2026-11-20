// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/Flow/GameTestGuards.h"
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
	/** 1.9.0 前的 blockers 在内存里改写成 guards：close / call / command 三种都能对上，enabled:false 变 disabled。 */
	TEST_METHOD(Parse_MigratesBlockersToGuards)
	{
		FGameTestRunConfig Config;
		FString Error;
		ASSERT_THAT(IsTrue(ParseRunConfig(TEXT(R"({"uiWaitTimeoutMs":0,"blockers":[
			{"widget":"WBP_Notice"},
			{"widget":"WBP_Guide_C","action":"call","function":"Skip","delayMs":300},
			{"widget":"/Game/UI/WBP_Reconnect","action":"command","command":"gm.Reconnect","enabled":false}]})"), Config, Error), Error));
		ASSERT_THAT(AreEqual(0, Config.UiWaitTimeoutMs));
		ASSERT_THAT(IsTrue(Config.bMigratedBlockers));
		ASSERT_THAT(AreEqual(3, Config.ParsedGuards.Num()));
		ASSERT_THAT(AreEqual(FString(TEXT("b1")), Config.ParsedGuards[0]->Id));
		ASSERT_THAT(IsTrue(Config.ParsedGuards[2]->bDisabled));
		const TArray<FString> Screens = GameTestRunConfig::GuardScreens(Config);
		ASSERT_THAT(AreEqual(2, Screens.Num(), TEXT("禁用的守护不算")));
		const TSharedPtr<FJsonObject> Json = GameTestRunConfig::ToJson(Config);
		ASSERT_THAT(IsFalse(Json->HasField(TEXT("blockers")), TEXT("写回时不再有 blockers")));
		ASSERT_THAT(IsTrue(Json->HasField(TEXT("guards"))));
	}

	/** blockers 和 guards 同时在：说明旧工具又写回了 blockers，报错让人处理。 */
	TEST_METHOD(Parse_RejectsBlockersWithGuards)
	{
		FGameTestRunConfig Config;
		FString Error;
		ASSERT_THAT(IsFalse(ParseRunConfig(TEXT(R"({"blockers":[{"widget":"WBP_A"}],"guards":[]})"), Config, Error)));
		ASSERT_THAT(IsTrue(Error.Contains(TEXT("guards")), Error));
	}

	/** 写错的守护在读配置时就报出来。 */
	TEST_METHOD(Parse_RejectsBadGuards)
	{
		FGameTestRunConfig Config;
		FString Error;
		ASSERT_THAT(IsFalse(ParseRunConfig(TEXT(R"({"guards":[{"id":"g1","do":[]}]})"), Config, Error)));
		ASSERT_THAT(IsFalse(ParseRunConfig(TEXT(R"({"maxCallDepth":0})"), Config, Error)));
		ASSERT_THAT(IsTrue(ParseRunConfig(TEXT(R"({"allowLuaInCond":true,"maxCallDepth":4})"), Config, Error), Error));
		ASSERT_THAT(IsTrue(Config.ScriptOptions.bAllowLuaInCond));
		ASSERT_THAT(AreEqual(4, Config.ScriptOptions.MaxCallDepth));
	}

	/** 面板、别的线程加的字段（api、panelNote）写回时原样留着。 */
	TEST_METHOD(ToJson_KeepsUnknownFields)
	{
		FGameTestRunConfig Config;
		FString Error;
		ASSERT_THAT(IsTrue(ParseRunConfig(TEXT(R"({"uiWaitTimeoutMs":15000,"api":["Scripts/GameTestApi"],"panelNote":{"a":1}})"), Config, Error), Error));
		const TSharedPtr<FJsonObject> Json = GameTestRunConfig::ToJson(Config);
		ASSERT_THAT(IsTrue(Json->HasField(TEXT("api"))));
		ASSERT_THAT(IsTrue(Json->HasField(TEXT("panelNote"))));

		TSharedRef<FJsonObject> Incoming = MakeShared<FJsonObject>();
		Incoming->SetNumberField(TEXT("uiWaitTimeoutMs"), 1000);
		GameTestRunConfig::PreserveUnknown(*Incoming, *Json);
		ASSERT_THAT(IsTrue(Incoming->HasField(TEXT("api")), TEXT("gt.config 写回不丢 api")));
		ASSERT_THAT(AreEqual(1000, static_cast<int32>(Incoming->GetNumberField(TEXT("uiWaitTimeoutMs")))));
	}

	/** 2.0.0：identRules / closeRules / refDefaults / allowDestroy 读得进、写得回；1.x 的 itemKeys / objectKeys 报 settings_old。 */
	TEST_METHOD(Parse_RefSettings)
	{
		FGameTestRunConfig Config;
		FString Error;
		ASSERT_THAT(IsTrue(ParseRunConfig(TEXT(R"({"identRules":[{"class":"WBP_BagSlot","ident":["@item.ItemId"]}],"closeRules":[{"class":"WBP_Shop","how":[{"by":"call","function":"CloseShop"}]}],"refDefaults":{"many":"first","closeVerifyMs":3000,"tol":{"float":0.01}},"allowDestroy":true})"), Config, Error), Error));
		const TSharedPtr<FJsonObject> Json = GameTestRunConfig::ToJson(Config);
		for (const TCHAR* Field : { TEXT("identRules"), TEXT("closeRules"), TEXT("refDefaults"), TEXT("allowDestroy") })
		{
			ASSERT_THAT(IsTrue(Json->HasField(Field), Field));
		}
		FGameTestRunConfig Again;
		ASSERT_THAT(IsTrue(GameTestRunConfig::FromJson(*Json, Again, Error), Error));
		ASSERT_THAT(AreEqual(FString(TEXT("first")), Again.ScriptOptions.RefMany));
		ASSERT_THAT(AreEqual(3000, Again.ScriptOptions.CloseVerifyMs));
		ASSERT_THAT(IsTrue(Again.ScriptOptions.bAllowDestroy));

		ASSERT_THAT(IsFalse(ParseRunConfig(TEXT(R"({"itemKeys":[]})"), Config, Error)));
		ASSERT_THAT(IsTrue(Error.Contains(TEXT("settings_old")), Error));
		ASSERT_THAT(IsFalse(ParseRunConfig(TEXT(R"({"refDefaults":{"many":"random"}})"), Config, Error)));
	}

	TEST_METHOD(Parse_MissingWaitMeansUseSettings)
	{
		FGameTestRunConfig Config;
		FString Error;
		ASSERT_THAT(IsTrue(ParseRunConfig(TEXT(R"({})"), Config, Error), Error));
		ASSERT_THAT(AreEqual(-1, Config.UiWaitTimeoutMs));
		ASSERT_THAT(AreEqual(-1, Config.UiStepGapMs));
	}

	/** UI 操作间隔：0 合法（不隔），负数、超上限、不是整数都拒绝，写回再读一致。 */
	TEST_METHOD(Parse_UiStepGap)
	{
		FGameTestRunConfig Config;
		FString Error;
		ASSERT_THAT(IsTrue(ParseRunConfig(TEXT(R"({"uiStepGapMs":0})"), Config, Error), Error));
		ASSERT_THAT(AreEqual(0, Config.UiStepGapMs));
		ASSERT_THAT(IsTrue(ParseRunConfig(TEXT(R"({"uiStepGapMs":1500})"), Config, Error), Error));
		FGameTestRunConfig Again;
		ASSERT_THAT(IsTrue(GameTestRunConfig::FromJson(*GameTestRunConfig::ToJson(Config), Again, Error), Error));
		ASSERT_THAT(AreEqual(1500, Again.UiStepGapMs));
		ASSERT_THAT(IsFalse(ParseRunConfig(TEXT(R"({"uiStepGapMs":-1})"), Config, Error)));
		ASSERT_THAT(IsFalse(ParseRunConfig(TEXT(R"({"uiStepGapMs":600000})"), Config, Error)));
	}

	TEST_METHOD(Parse_RejectsBadWait)
	{
		FGameTestRunConfig Config;
		FString Error;
		ASSERT_THAT(IsFalse(ParseRunConfig(TEXT(R"({"uiWaitTimeoutMs":-5})"), Config, Error)));
	}

	TEST_METHOD(ToJson_RoundTrips)
	{
		FGameTestRunConfig Config;
		FString Error;
		ASSERT_THAT(IsTrue(ParseRunConfig(TEXT(R"({"uiWaitTimeoutMs":15000,"guards":[{"id":"guide","name":"新手引导","when":{"obj":{"class":"WBP_Guide"},"prop":":onScreen","op":"is","value":true},"do":[{"action":"obj.close","obj":{"class":"WBP_Guide"},"how":[{"by":"call","function":"Skip"}]}]}]})"), Config, Error), Error));
		FGameTestRunConfig Again;
		ASSERT_THAT(IsTrue(GameTestRunConfig::FromJson(*GameTestRunConfig::ToJson(Config), Again, Error), Error));
		ASSERT_THAT(AreEqual(15000, Again.UiWaitTimeoutMs));
		ASSERT_THAT(AreEqual(1, Again.ParsedGuards.Num()));
		ASSERT_THAT(AreEqual(FString(TEXT("guide")), Again.ParsedGuards[0]->Id));
	}
};

#endif
