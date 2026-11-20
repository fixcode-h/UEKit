// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/GameTestAction.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestTypes.h"

#if WITH_AUTOMATION_WORKER

namespace
{
	TSharedPtr<FJsonObject> ParseStepText(const FString& InText)
	{
		FString Error;
		const TSharedPtr<FJsonObject> Object = GameTestJson::ParseObjectFromText(InText, Error);
		return Object;
	}
}

TEST_CLASS_WITH_FLAGS(GameTestActionTest, "GameTest.Action",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/**
	 * 解析失败时能定位到哪一步——这条用例是给"为什么这条步骤过不了解析"用的。
	 * 注意断言消息里不要直接引用 Errors 数组：函数实参求值顺序未定，
	 * 会把"填充之前"的空数组格式化进消息里（实测踩过，消息看起来像"没有错误"）。
	 */
	TEST_METHOD(ParseStep_ReportsWhichStageFailed)
	{
		const TSharedPtr<FJsonObject> Object = ParseStepText(TEXT(R"({"action":"ping"})"));
		ASSERT_THAT(IsTrue(Object.IsValid(), TEXT("第一步：JSON 本身要能解析")));

		FString ActionText;
		ASSERT_THAT(IsTrue(GameTestJson::TryGetString(*Object, TEXT("action"), ActionText),
			TEXT("第二步：action 字段要能读到")));
		ASSERT_THAT(IsTrue(GameTestActions::Find(FName(*ActionText)) != nullptr,
			FString::Printf(TEXT("第三步：'%s' 要在词汇表里"), *ActionText)));

		FGameTestStep Step;
		TArray<FString> Errors;
		const bool bParsed = GameTestActions::ParseStep(Object, 0, Step, Errors);
		ASSERT_THAT(IsTrue(bParsed, FString::Printf(TEXT("第四步：ParseStep 应通过；错误=%s"),
			*FString::Join(Errors, TEXT("；")))));
	}

	TEST_METHOD(ParseStep_AcceptsValidStep)
	{
		FGameTestStep Step;
		TArray<FString> Errors;
		const bool bParsed = GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"click","target":{"obj":{"class":"WBP_MainMenu"},"part":"BtnStart"},"note":"点开始"})")),
			3, Step, Errors);
		ASSERT_THAT(IsTrue(bParsed, FString::Join(Errors, TEXT("；"))));

		ASSERT_THAT(IsTrue(Step.Action == TEXT("click"), *Step.Action.ToString()));
		// 部件写法在步骤里存成内部的 @@ref: + 紧凑 JSON（GameTestHandlers::TargetText），解析时按对象引用走
		ASSERT_THAT(IsTrue(Step.Target.StartsWith(TEXT("@@ref:")) && Step.Target.Contains(TEXT("BtnStart")), *Step.Target));
		ASSERT_THAT(IsTrue(Step.Note == TEXT("点开始"), *Step.Note));
		ASSERT_THAT(IsTrue(Step.Index == 3, TEXT("步骤序号要带进记录")));
		ASSERT_THAT(IsTrue(Step.FailMode == EGameTestFailMode::Stop, TEXT("默认失败即停")));
	}

	TEST_METHOD(ParseStep_RejectsSelectorString)
	{
		// v4 起字符串选择器不认（docs/game-test-ref.md 5.1）：报 selector_removed，说清楚改成什么
		FGameTestStep Step;
		TArray<FString> Errors;
		ASSERT_THAT(IsFalse(GameTestActions::ParseStep(ParseStepText(TEXT(R"({"action":"click","target":"WBP_MainMenu:Root/BtnStart"})")), 0, Step, Errors)));
		ASSERT_THAT(IsTrue(FString::Join(Errors, TEXT("；")).Contains(TEXT("selector_removed")), *FString::Join(Errors, TEXT("；"))));
	}

	TEST_METHOD(ParseStep_AcceptsNamedRefAndHandle)
	{
		// objects 里的名字、@句柄 都能当 target
		FGameTestStep Step;
		TArray<FString> Errors;
		ASSERT_THAT(IsTrue(GameTestActions::ParseStep(ParseStepText(TEXT(R"({"action":"click","target":{"obj":"shop","part":"BtnBuy"}})")), 0, Step, Errors), FString::Join(Errors, TEXT("；"))));
		ASSERT_THAT(IsTrue(GameTestActions::ParseStep(ParseStepText(TEXT(R"({"action":"click","target":"@btn"})")), 0, Step, Errors), FString::Join(Errors, TEXT("；"))));
		ASSERT_THAT(IsTrue(Step.Target == TEXT("@btn"), *Step.Target));
	}

	TEST_METHOD(ParseStep_RemovedActionsSayWhatToUse)
	{
		// invoke / find / widget.close / widget.call 删掉：报 action_removed 并说改用哪个
		for (const TCHAR* Old : { TEXT("invoke"), TEXT("find"), TEXT("widget.close"), TEXT("widget.call") })
		{
			FGameTestStep Step;
			TArray<FString> Errors;
			ASSERT_THAT(IsFalse(GameTestActions::ParseStep(ParseStepText(FString::Printf(TEXT("{\"action\":\"%s\"}"), Old)), 0, Step, Errors)));
			const FString Joined = FString::Join(Errors, TEXT("；"));
			ASSERT_THAT(IsTrue(Joined.Contains(TEXT("action_removed")) && Joined.Contains(TEXT("obj.")), *Joined));
		}
	}

	TEST_METHOD(ParseStep_OldAssertFieldsReportCondRemoved)
	{
		FGameTestStep Step;
		TArray<FString> Errors;
		ASSERT_THAT(IsFalse(GameTestActions::ParseStep(ParseStepText(TEXT(R"({"action":"assert","target":{"obj":"shop"},"visible":true})")), 0, Step, Errors)));
		ASSERT_THAT(IsTrue(FString::Join(Errors, TEXT("；")).Contains(TEXT("cond_removed")), *FString::Join(Errors, TEXT("；"))));
	}

	TEST_METHOD(ParseStep_RejectsMissingAction)
	{
		FGameTestStep Step;
		TArray<FString> Errors;
		ASSERT_THAT(IsFalse(GameTestActions::ParseStep(ParseStepText(TEXT(R"({"target":"x"})")), 0, Step, Errors)));
		ASSERT_THAT(IsTrue(Errors.Num() > 0 && Errors[0].Contains(TEXT("action")), *FString::Join(Errors, TEXT("；"))));
	}

	TEST_METHOD(ParseStep_SuggestsCandidatesForMisspelledAction)
	{
		FGameTestStep Step;
		TArray<FString> Errors;
		ASSERT_THAT(IsFalse(GameTestActions::ParseStep(ParseStepText(TEXT(R"({"action":"clik","target":"x"})")), 0, Step, Errors)));
		ASSERT_THAT(IsTrue(FString::Join(Errors, TEXT("；")).Contains(TEXT("click")), *FString::Join(Errors, TEXT("；"))));
	}

	TEST_METHOD(ParseStep_RejectsUnknownFieldAndListsAvailable)
	{
		FGameTestStep Step;
		TArray<FString> Errors;
		// targe 是 target 的典型笔误：静默忽略它会让这一步点到别的东西上。
		ASSERT_THAT(IsFalse(GameTestActions::ParseStep(ParseStepText(TEXT(R"({"action":"click","targe":"x"})")), 0, Step, Errors)));
		const FString Joined = FString::Join(Errors, TEXT("；"));
		ASSERT_THAT(IsTrue(Joined.Contains(TEXT("targe")), *Joined));
		ASSERT_THAT(IsTrue(Joined.Contains(TEXT("target")), *Joined));
	}

	TEST_METHOD(ParseStep_RejectsMissingTargetWhenRequired)
	{
		FGameTestStep Step;
		TArray<FString> Errors;
		ASSERT_THAT(IsFalse(GameTestActions::ParseStep(ParseStepText(TEXT(R"({"action":"click"})")), 0, Step, Errors)));
		ASSERT_THAT(IsTrue(FString::Join(Errors, TEXT("；")).Contains(TEXT("target")), *FString::Join(Errors, TEXT("；"))));
	}

	TEST_METHOD(ParseStep_AcceptsTargetlessActionWithoutTarget)
	{
		FGameTestStep Step;
		TArray<FString> Errors;
		const bool bParsed = GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"pie.start","width":1600,"height":900})")), 0, Step, Errors);
		ASSERT_THAT(IsTrue(bParsed, FString::Join(Errors, TEXT("；"))));
	}

	TEST_METHOD(ParseStep_OnFailOnlyAcceptsStopAndContinue)
	{
		FGameTestStep Step;
		TArray<FString> Errors;
		const bool bParsed = GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"ping","onFail":"continue"})")), 0, Step, Errors);
		ASSERT_THAT(IsTrue(bParsed, FString::Join(Errors, TEXT("；"))));
		ASSERT_THAT(IsTrue(Step.FailMode == EGameTestFailMode::Continue, TEXT("continue 要生效")));

		TArray<FString> BadErrors;
		ASSERT_THAT(IsFalse(GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"ping","onFail":"whatever"})")), 1, Step, BadErrors)));
	}

	TEST_METHOD(ParseStep_CatchesMisspelledActionSpecificField)
	{
		FGameTestStep Step;
		TArray<FString> Errors;
		// pie.start 的白名单里是 map/width/height，widht 是笔误。
		ASSERT_THAT(IsFalse(GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"pie.start","widht":1600})")), 0, Step, Errors)));
		ASSERT_THAT(IsTrue(FString::Join(Errors, TEXT("；")).Contains(TEXT("width")), *FString::Join(Errors, TEXT("；"))));
	}

	TEST_METHOD(ParseStep_ProbeRejectsUnknownFieldAndListsAvailable)
	{
		FGameTestStep Step;
		TArray<FString> Errors;
		// poinX 是 pointX 的典型笔误：静默忽略它会让探测点悄悄回到默认中心，结论跟着变样。
		ASSERT_THAT(IsFalse(GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"probe","target":{"obj":{"class":"WBP_A"},"part":"Btn"},"poinX":0.8})")), 0, Step, Errors)));
		const FString Joined = FString::Join(Errors, TEXT("；"));
		ASSERT_THAT(IsTrue(Joined.Contains(TEXT("poinX")), *Joined));
		ASSERT_THAT(IsTrue(Joined.Contains(TEXT("pointX")), *Joined));
	}

	TEST_METHOD(ParseStep_AcceptsProbeWithExpectAndPoint)
	{
		FGameTestStep Step;
		TArray<FString> Errors;
		const bool bParsed = GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"probe","target":{"obj":{"class":"WBP_A"},"part":"TargetButton"},"pointX":0.8,"expect":"*:Root/Mask"})")),
			0, Step, Errors);
		ASSERT_THAT(IsTrue(bParsed, FString::Join(Errors, TEXT("；"))));
	}

	TEST_METHOD(ParseStep_AcceptsInputActions)
	{
		// 三个输入动作都不需要 target（它们作用于世界而不是 UMG），字段白名单各不相同。
		FGameTestStep Step;

		TArray<FString> StartErrors;
		ASSERT_THAT(IsTrue(GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"input.start","asset":"/Game/Input/IA_Throttle","value":1.0})")), 0, Step, StartErrors),
			FString::Join(StartErrors, TEXT("；"))));

		TArray<FString> StopErrors;
		ASSERT_THAT(IsTrue(GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"input.stop","asset":"/Game/Input/IA_Throttle"})")), 1, Step, StopErrors),
			FString::Join(StopErrors, TEXT("；"))));

		TArray<FString> CursorErrors;
		ASSERT_THAT(IsTrue(GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"input.cursor","x":0.5,"y":0.25})")), 2, Step, CursorErrors),
			FString::Join(CursorErrors, TEXT("；"))));
	}

	TEST_METHOD(ParseStep_InputActionsUseAssetNotAction)
	{
		// 资产路径的字段名必须是 `asset`：公共字段里的 `action` 是**动作名**，同名会让解析器
		// 把这个路径当成动作名去查词汇表，报"未知动作 '/Game/Input/IA_Throttle'"（实测踩过）。
		// 直接断言白名单，别用"重名 key 谁的优先级高"这种依赖解析器细节的写法。
		const FGameTestActionInfo* Start = GameTestActions::Find(TEXT("input.start"));
		const FGameTestActionInfo* Stop = GameTestActions::Find(TEXT("input.stop"));
		ASSERT_THAT(IsNotNull(Start, TEXT("input.start 要在词汇表里")));
		ASSERT_THAT(IsNotNull(Stop, TEXT("input.stop 要在词汇表里")));

		// Params 是逗号分隔的字段清单（不是数组），用子串判断。
		ASSERT_THAT(IsTrue(FString(Start->Params).Contains(TEXT("asset")), Start->Params));
		ASSERT_THAT(IsTrue(FString(Stop->Params).Contains(TEXT("asset")), Stop->Params));

		// 正例：字段名写 asset 就通过。
		FGameTestStep Step;
		TArray<FString> GoodErrors;
		ASSERT_THAT(IsTrue(GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"input.start","asset":"/Game/Input/IA_Throttle"})")), 0, Step, GoodErrors),
			FString::Join(GoodErrors, TEXT("；"))));
	}

	TEST_METHOD(ParseStep_InputStartRejectsUnknownField)
	{
		// valu 是 value 的典型笔误：静默忽略它会让注入悄悄用默认值 1.0，结论跟着变样。
		FGameTestStep Step;
		TArray<FString> Errors;
		ASSERT_THAT(IsFalse(GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"input.start","asset":"x","valu":1.0})")), 0, Step, Errors)));
		const FString Joined = FString::Join(Errors, TEXT("；"));
		ASSERT_THAT(IsTrue(Joined.Contains(TEXT("valu")), *Joined));
		ASSERT_THAT(IsTrue(Joined.Contains(TEXT("value")), *Joined));
	}

	TEST_METHOD(ParseStep_WaitActorNearAcceptsFields)
	{
		FGameTestStep Step;
		TArray<FString> Errors;
		const bool bParsed = GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"wait","until":"actorNear","actor":"ATank","point":[100,200,0],"radius":300})")),
			0, Step, Errors);
		ASSERT_THAT(IsTrue(bParsed, FString::Join(Errors, TEXT("；"))));
	}

	TEST_METHOD(ParseStep_WaitActorNearRejectsUnknownField)
	{
		// radus 是 radius 的笔误：静默忽略会让这一步等一个永远不成立的半径。
		FGameTestStep Step;
		TArray<FString> Errors;
		ASSERT_THAT(IsFalse(GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"wait","until":"actorNear","actor":"ATank","radus":300})")), 0, Step, Errors)));
		const FString Joined = FString::Join(Errors, TEXT("；"));
		ASSERT_THAT(IsTrue(Joined.Contains(TEXT("radus")), *Joined));
		ASSERT_THAT(IsTrue(Joined.Contains(TEXT("radius")), *Joined));
	}

	TEST_METHOD(ParseStep_AcceptsActorGet)
	{
		FGameTestStep Step;
		TArray<FString> Errors;
		ASSERT_THAT(IsTrue(GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"actor.get","class":"ATank"})")), 0, Step, Errors),
			FString::Join(Errors, TEXT("；"))));

		TArray<FString> BothErrors;
		ASSERT_THAT(IsTrue(GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"actor.get","class":"ATank","tag":"Player"})")), 1, Step, BothErrors),
			FString::Join(BothErrors, TEXT("；"))));
	}

	TEST_METHOD(ParseStep_EveryVocabularyEntryRoundTrips)
	{
		// 词汇表与实现表的对应关系由运行期保证；这里至少保证"表自身自洽"：
		// 每个动作都能在没有参数的情况下通过解析（需要 target 的补一个占位）。
		for (const FGameTestActionInfo& Info : GameTestActions::GetAll())
		{
			const FString Text = Info.bNeedsTarget
				? FString::Printf(TEXT("{\"action\":\"%s\",\"target\":\"@placeholder\"}"), *Info.Name.ToString())
				: FString::Printf(TEXT("{\"action\":\"%s\"}"), *Info.Name.ToString());

			FGameTestStep Step;
			TArray<FString> Errors;
			const bool bParsed = GameTestActions::ParseStep(ParseStepText(Text), 0, Step, Errors);
			ASSERT_THAT(IsTrue(bParsed, FString::Printf(TEXT("%s：%s"), *Info.Name.ToString(), *FString::Join(Errors, TEXT("；")))));
		}
	}

	TEST_METHOD(Registry_EveryActionHasHandler)
	{
		// 说明和实现一起登记：表里的每个动作都要能找到实现。
		for (const FGameTestActionInfo& Info : GameTestActions::GetAll())
		{
			ASSERT_THAT(IsTrue(GameTestActions::FindHandler(Info.Name) != nullptr,
				FString::Printf(TEXT("%s 没有实现"), *Info.Name.ToString())));
		}
	}

	TEST_METHOD(Registry_MissingOptionalActionNamesThePlugin)
	{
		// 可选模块没加载时，写了它的动作要说清楚缺哪个插件，而不是「未知动作」。
		const FGameTestActionInfo* Original = GameTestActions::Find(TEXT("stack.pop"));
		const TOptional<FGameTestActionInfo> Saved = Original != nullptr ? TOptional<FGameTestActionInfo>(*Original) : TOptional<FGameTestActionInfo>();
		GameTestActions::Unregister(TEXT("stack.pop"));

		FGameTestStep Step;
		TArray<FString> Errors;
		const bool bParsed = GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"stack.pop","target":"@x"})")), 0, Step, Errors);
		const FString Joined = FString::Join(Errors, TEXT("；"));

		if (Saved.IsSet())
		{
			GameTestActions::Register(Saved.GetValue());
		}

		ASSERT_THAT(IsFalse(bParsed));
		ASSERT_THAT(IsTrue(Joined.Contains(TEXT("CommonUI")), *Joined));
	}

	TEST_METHOD(ParseStep_KeyAcceptsKeyAndPhase)
	{
		FGameTestStep Step;
		TArray<FString> Errors;
		ASSERT_THAT(IsTrue(GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"key","key":"SpaceBar","phase":"press"})")), 0, Step, Errors),
			FString::Join(Errors, TEXT("；"))));
	}

	TEST_METHOD(ParseStep_PieStartAcceptsNetModeAndPlayers)
	{
		FGameTestStep Step;
		TArray<FString> Errors;
		ASSERT_THAT(IsTrue(GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"pie.start","netMode":"listenServer","players":2})")), 0, Step, Errors),
			FString::Join(Errors, TEXT("；"))));
	}

	TEST_METHOD(ParseStep_ActorGetAcceptsClientAndLocallyControlled)
	{
		FGameTestStep Step;
		TArray<FString> Errors;
		ASSERT_THAT(IsTrue(GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"actor.get","class":"ATank","client":0,"locallyControlled":false})")), 0, Step, Errors),
			FString::Join(Errors, TEXT("；"))));
	}

	TEST_METHOD(ParseStep_NetCompareAcceptsSidesAndTolerance)
	{
		FGameTestStep Step;
		TArray<FString> Errors;
		ASSERT_THAT(IsTrue(GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"net.compare","class":"ATank","tolerance":200,"from":{"client":"server","locallyControlled":false},"to":{"client":0,"locallyControlled":true},"timeoutMs":5000})")),
			0, Step, Errors),
			FString::Join(Errors, TEXT("；"))));
	}

	TEST_METHOD(ParseStep_NetCompareRejectsMisspelledTolerance)
	{
		// tolerence 是 tolerance 的笔误：静默忽略会让"必填缺失"推迟到运行期才报，早在这里拦下更好。
		FGameTestStep Step;
		TArray<FString> Errors;
		ASSERT_THAT(IsFalse(GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"net.compare","class":"ATank","tolerence":200})")), 0, Step, Errors)));
		const FString Joined = FString::Join(Errors, TEXT("；"));
		ASSERT_THAT(IsTrue(Joined.Contains(TEXT("tolerence")), *Joined));
		ASSERT_THAT(IsTrue(Joined.Contains(TEXT("tolerance")), *Joined));
	}

	TEST_METHOD(ParseStep_ConsoleAcceptsExpectContains)
	{
		// console 的输出断言（expectContains）是把"世界状态"变成场景可判定观察的通道。
		FGameTestStep Step;
		TArray<FString> Errors;
		ASSERT_THAT(IsTrue(GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"console","command":"MyGame.DebugState","expectContains":"Wrecked=true"})")), 0, Step, Errors),
			FString::Join(Errors, TEXT("；"))));
	}

	TEST_METHOD(ParseStep_ConsoleRejectsMisspelledExpectContains)
	{
		// expectContain 是 expectContains 的笔误：静默忽略会让"该失败的断言"变成永远绿色。
		FGameTestStep Step;
		TArray<FString> Errors;
		ASSERT_THAT(IsFalse(GameTestActions::ParseStep(
			ParseStepText(TEXT(R"({"action":"console","command":"x","expectContain":"y"})")), 0, Step, Errors)));
		const FString Joined = FString::Join(Errors, TEXT("；"));
		ASSERT_THAT(IsTrue(Joined.Contains(TEXT("expectContain")), *Joined));
		ASSERT_THAT(IsTrue(Joined.Contains(TEXT("expectContains")), *Joined));
	}
};

#endif	// WITH_AUTOMATION_WORKER
