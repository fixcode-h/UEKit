// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/GameTestAction.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestTypes.h"
#include "Observe/GameTestCondition.h"
#include "Operate/GameTestSkipIf.h"
#include "Record/GameTestRecordState.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_AUTOMATION_WORKER

namespace GameTestSkipIfTestDetail
{
	TSharedPtr<FJsonObject> Parse(const TCHAR* InText)
	{
		FString Error;
		return GameTestJson::ParseObjectFromText(InText, Error);
	}

	bool ParseStep(const TCHAR* InText, FString& OutErrors)
	{
		FGameTestStep Step;
		TArray<FString> Errors;
		const bool bOk = GameTestActions::ParseStep(Parse(InText), 0, Step, Errors);
		OutErrors = FString::Join(Errors, TEXT("；"));
		return bOk;
	}

	TArray<TSharedRef<FJsonObject>> Steps(const TArray<const TCHAR*>& InTexts)
	{
		TArray<TSharedRef<FJsonObject>> Out;
		for (const TCHAR* Text : InTexts)
		{
			const TSharedPtr<FJsonObject> Step = Parse(Text);
			Out.Add(Step.ToSharedRef());
		}
		return Out;
	}
}

TEST_CLASS_WITH_FLAGS(GameTestSkipIfTest, "GameTest.SkipIf",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 步骤里的 skipIf（v4：{cond, also, settleMs, verify, verifyTimeoutMs}）：条件和配置字段都认，写错的报出来。 */
	TEST_METHOD(ValidateStep_AcceptsConditionsAndConfig)
	{
		using namespace GameTestSkipIfTestDetail;
		FString Errors;
		ASSERT_THAT(IsTrue(ParseStep(TEXT(R"({"action":"click","target":{"obj":"shop","part":"Btn"},"skipIf":{"cond":{"obj":"shop","prop":"@/ChkAuto:checked","op":"is","value":true}}})"), Errors), Errors));
		ASSERT_THAT(IsTrue(ParseStep(TEXT(R"({"action":"click","target":{"obj":"shop","part":"Btn"},"skipIf":{"cond":{"obj":"shop","prop":"@:onScreen","op":"is","value":false},"also":2,"settleMs":500,"verify":false}})"), Errors), Errors));
		ASSERT_THAT(IsTrue(ParseStep(TEXT(R"({"action":"click","target":{"obj":"shop","part":"Btn"},"skipIf":{"cond":"1 == 1"}})"), Errors), Errors));
	}

	TEST_METHOD(ValidateStep_RejectsMistakes)
	{
		using namespace GameTestSkipIfTestDetail;
		FString Errors;
		// 没有条件
		ASSERT_THAT(IsFalse(ParseStep(TEXT(R"({"action":"click","target":{"obj":"shop","part":"Btn"},"skipIf":{"also":1}})"), Errors)));
		// v3 的写法（判断字段直接写在 skipIf 上）：cond_removed
		ASSERT_THAT(IsFalse(ParseStep(TEXT(R"({"action":"click","target":{"obj":"shop","part":"Btn"},"skipIf":{"checked":true}})"), Errors)));
		ASSERT_THAT(IsTrue(Errors.Contains(TEXT("cond_removed")), Errors));
		// 不是对象
		ASSERT_THAT(IsFalse(ParseStep(TEXT(R"({"action":"click","target":{"obj":"shop","part":"Btn"},"skipIf":true})"), Errors)));
		// also 超范围
		ASSERT_THAT(IsFalse(ParseStep(TEXT(R"({"action":"click","target":{"obj":"shop","part":"Btn"},"skipIf":{"cond":"1 == 1","also":99}})"), Errors)));
	}

	/** 运行配置 / 场景的 skipIf 段：false 关掉，对象逐项改。 */
	TEST_METHOD(ApplyOptions_BoolAndObject)
	{
		FGameTestSkipOptions Options;
		FString Error;
		ASSERT_THAT(IsTrue(GameTestSkipIf::ApplyOptions(FJsonValueBoolean(false), Options, Error), Error));
		ASSERT_THAT(IsFalse(Options.bEnabled));
		const TSharedPtr<FJsonObject> Object = GameTestSkipIfTestDetail::Parse(TEXT(R"({"enabled":true,"settleMs":800,"verify":false,"verifyTimeoutMs":9000})"));
		ASSERT_THAT(IsTrue(GameTestSkipIf::ApplyOptions(FJsonValueObject(Object), Options, Error), Error));
		ASSERT_THAT(IsTrue(Options.bEnabled));
		ASSERT_THAT(AreEqual(800, Options.SettleMs));
		ASSERT_THAT(IsFalse(Options.bVerify));
		ASSERT_THAT(AreEqual(9000, Options.VerifyTimeoutMs));
		const TSharedPtr<FJsonObject> Bad = GameTestSkipIfTestDetail::Parse(TEXT(R"({"settleMs":-1})"));
		ASSERT_THAT(IsFalse(GameTestSkipIf::ApplyOptions(FJsonValueObject(Bad), Options, Error)));
	}

	/** 步骤字段盖在选项上：做完核对、核对时长、连带。 */
	TEST_METHOD(StepFieldsOverrideOptions)
	{
		FGameTestSkipOptions Options;
		const TSharedPtr<FJsonObject> SkipIf = GameTestSkipIfTestDetail::Parse(TEXT(R"({"cond":"$a","verify":false,"also":3})"));
		ASSERT_THAT(IsFalse(GameTestSkipIf::ShouldVerify(*SkipIf, Options)));
		ASSERT_THAT(AreEqual(3, GameTestSkipIf::Also(*SkipIf)));
		ASSERT_THAT(AreEqual(GameTestSkipIf::DefaultVerifyTimeoutMs, GameTestSkipIf::VerifyTimeoutMs(*SkipIf, Options)));
		Options.bVerify = false;
		const TSharedPtr<FJsonObject> Plain = GameTestSkipIfTestDetail::Parse(TEXT(R"({"cond":"$a"})"));
		ASSERT_THAT(IsFalse(GameTestSkipIf::ShouldVerify(*Plain, Options)));
	}

	/** 「不存在 / 不可见」要等界面打开才算数：识别出反面条件（含 any 里的）。 */
	TEST_METHOD(Condition_HasNegative)
	{
		ASSERT_THAT(IsTrue(GameTestCondition::HasNegative(*GameTestSkipIfTestDetail::Parse(TEXT(R"({"visible":false})")))));
		ASSERT_THAT(IsTrue(GameTestCondition::HasNegative(*GameTestSkipIfTestDetail::Parse(TEXT(R"({"any":[{"checked":true},{"exists":false}]})")))));
		ASSERT_THAT(IsFalse(GameTestCondition::HasNegative(*GameTestSkipIfTestDetail::Parse(TEXT(R"({"visible":true,"textLike":"已领取"})")))));
	}

	/** 目标没解析到时只有 exists:false 成立。 */
	TEST_METHOD(Condition_UnresolvedOnlyExistsFalseHolds)
	{
		FGameTestConditionCheck Check;
		GameTestCondition::Check(*GameTestSkipIfTestDetail::Parse(TEXT(R"({"exists":false})")), nullptr, TEXT("没找到"), Check);
		ASSERT_THAT(IsTrue(Check.Holds()));
		FGameTestConditionCheck Other;
		GameTestCondition::Check(*GameTestSkipIfTestDetail::Parse(TEXT(R"({"checked":true})")), nullptr, TEXT("没找到"), Other);
		ASSERT_THAT(IsFalse(Other.Holds()));
	}

	/** 录制推断：只变了数字的文字、变空的不算；名字像「已领 / 勾」的更像目的。 */
	TEST_METHOD(RecordState_TextAndNames)
	{
		ASSERT_THAT(IsTrue(FGameTestRecordState::IsMeaningfulTextChange(TEXT("领取"), TEXT("已领取"))));
		ASSERT_THAT(IsFalse(FGameTestRecordState::IsMeaningfulTextChange(TEXT("剩余 10 秒"), TEXT("剩余 9 秒"))));
		ASSERT_THAT(IsFalse(FGameTestRecordState::IsMeaningfulTextChange(TEXT("领取"), TEXT(""))));
		ASSERT_THAT(IsFalse(FGameTestRecordState::IsMeaningfulTextChange(TEXT("3"), TEXT("4"))));
		ASSERT_THAT(IsTrue(FGameTestRecordState::IsPurposeName(TEXT("ImgCheckMark"))));
		ASSERT_THAT(IsTrue(FGameTestRecordState::IsPurposeName(TEXT("bClaimed"))));
		ASSERT_THAT(IsFalse(FGameTestRecordState::IsPurposeName(TEXT("BtnStart"))));
	}

	/** 候选按分排，最高分 ≥ 7 才自动写；同分时勾选排在文字前面。 */
	TEST_METHOD(RecordState_SortAndPick)
	{
		auto Make = [](const TCHAR* InKind, int32 InScore) {
			FGameTestStateCandidate Candidate;
			Candidate.Kind = InKind;
			Candidate.Score = InScore;
			return Candidate;
		};
		TArray<FGameTestStateCandidate> Low = { Make(TEXT("visible"), 4), Make(TEXT("enabled"), 6) };
		ASSERT_THAT(AreEqual(INDEX_NONE, FGameTestRecordState::SortAndPick(Low)));
		ASSERT_THAT(AreEqual(FString(TEXT("enabled")), Low[0].Kind));
		TArray<FGameTestStateCandidate> Tie = { Make(TEXT("text"), 8), Make(TEXT("checked"), 8), Make(TEXT("visible"), 4) };
		ASSERT_THAT(AreEqual(0, FGameTestRecordState::SortAndPick(Tie)));
		ASSERT_THAT(AreEqual(FString(TEXT("checked")), Tie[0].Kind));
	}

	/** 连带跳过：后面紧挨着、目标在点完打开的界面上的步骤（wait 一起），阻挡界面不算。 */
	TEST_METHOD(RecordState_ComputeAlso)
	{
		const TArray<TSharedRef<FJsonObject>> Steps = GameTestSkipIfTestDetail::Steps({
			TEXT(R"({"action":"click","target":"WBP_Reward:Root/BtnClaim"})"),
			TEXT(R"({"action":"wait","seconds":1})"),
			TEXT(R"({"action":"click","target":"WBP_RewardPopup:Root/BtnOk"})"),
			TEXT(R"({"action":"click","target":"WBP_Reward:Root/BtnClose"})"),
		});
		ASSERT_THAT(AreEqual(2, FGameTestRecordState::ComputeAlso(Steps, 0, { TEXT("WBP_RewardPopup") }, {})));
		ASSERT_THAT(AreEqual(0, FGameTestRecordState::ComputeAlso(Steps, 0, { TEXT("WBP_RewardPopup") }, { TEXT("/Game/UI/WBP_RewardPopup") })));
		ASSERT_THAT(AreEqual(0, FGameTestRecordState::ComputeAlso(Steps, 0, {}, {})));
	}
};

#endif
