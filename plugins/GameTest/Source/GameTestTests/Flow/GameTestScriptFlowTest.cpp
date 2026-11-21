// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/Flow/GameTestScriptParser.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestRunner.h"
#include "Core/GameTestTypes.h"
#include "Core/Script/GameTestDiag.h"

#include "Dom/JsonObject.h"

#if WITH_AUTOMATION_WORKER

namespace GameTestScriptFlowTestLocal
{
	/** 场景 JSON → 解析好的脚本；诊断留在 OutSink 里给用例看。 */
	TSharedRef<FGameTestScript> ParseText(const TCHAR* InText, FGameTestDiagSink& OutSink)
	{
		FString Error;
		const TSharedPtr<FJsonObject> Json = GameTestJson::ParseObjectFromText(InText, Error);
		FGameTestScriptParser Parser(FString(), OutSink, FGameTestScriptOptions());
		return Parser.ParseScript(Json, TEXT("flow-test.json"));
	}

	bool HasCode(const FGameTestDiagSink& InSink, const TCHAR* InCode)
	{
		for (const FGameTestDiag& Diag : InSink.GetAll())
		{
			if (Diag.Code == FName(InCode))
			{
				return true;
			}
		}
		return false;
	}

	FString DescribeDiags(const FGameTestDiagSink& InSink)
	{
		TArray<FString> Lines;
		for (const FGameTestDiag& Diag : InSink.GetAll())
		{
			Lines.Add(Diag.ToLine());
		}
		return FString::Join(Lines, TEXT("；"));
	}

	/** 「id=状态」串：断言失败时看实际跑成了什么。 */
	FString DescribeReport(const FGameTestReport& InReport)
	{
		TArray<FString> Parts;
		for (const FGameTestStepRecord& Record : InReport.Steps)
		{
			Parts.Add(FString::Printf(TEXT("%s%s=%s"), *Record.Step.Label(), Record.Iter >= 0 ? *FString::Printf(TEXT("#%d"), Record.Iter) : TEXT(""),
				GameTestStatus::ToString(Record.Status)));
		}
		return FString::Join(Parts, TEXT(", "));
	}

	int32 CountById(const FGameTestReport& InReport, const TCHAR* InId, EGameTestStepStatus InStatus)
	{
		int32 Count = 0;
		for (const FGameTestStepRecord& Record : InReport.Steps)
		{
			Count += Record.Step.Id == InId && Record.Status == InStatus ? 1 : 0;
		}
		return Count;
	}

	/**
	 * 跑一份脚本直到结束：叶子只用 mark（不碰世界），所以每帧推进时间就能跑完；
	 * 条件都写 settleMs 0，不用等稳住。
	 */
	const FGameTestReport& RunToEnd(FGameTestRunner& InRunner, const TSharedRef<FGameTestScript>& InScript, FString& OutError)
	{
		FGameTestRunRequest Request;
		Request.Name = TEXT("flow-test");
		Request.RunId = TEXT("flow-test-1");
		Request.Script = InScript;
		Request.StepDelayMs = 0;
		Request.UiStepGapMs = 0;
		Request.DefaultStepTimeoutMs = 60000;
		FGameTestRuntimeState RuntimeState;
		if (InRunner.StartRun(Request, RuntimeState, nullptr, OutError))
		{
			for (int32 Frame = 0; Frame < 200 && InRunner.IsBusy(); ++Frame)
			{
				InRunner.Tick(Frame * 0.05);
			}
		}
		return InRunner.GetReport();
	}
}

TEST_CLASS_WITH_FLAGS(GameTestScriptFlowTest, "GameTest.Script",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 加载时就报的错：重复 id、循环 max 是负数（0 / 不写是不限，不报）、没声明的变量、块里拼错的字段。 */
	TEST_METHOD(Parse_ReportsScriptErrors)
	{
		using namespace GameTestScriptFlowTestLocal;
		FGameTestDiagSink Sink;
		ParseText(TEXT(R"({"kind":"gametest-scenario","schemaVersion":5,"name":"t","steps":[
			{"id":"a","action":"mark","text":"1"},
			{"id":"a","action":"mark","text":"2"},
			{"id":"l","action":"loop","until":{"expr":"true"},"max":-1,"do":[{"action":"mark","text":"x"}]},
			{"id":"l0","action":"loop","until":{"expr":"true"},"do":[{"action":"mark","text":"x"}]},
			{"action":"assert","expr":"$nope == 1"},
			{"action":"group","name":"g","doo":[]}]})"), Sink);
		ASSERT_THAT(IsTrue(HasCode(Sink, TEXT("dup_id")), DescribeDiags(Sink)));
		ASSERT_THAT(IsTrue(HasCode(Sink, TEXT("bad_type")), DescribeDiags(Sink)));
		ASSERT_THAT(IsTrue(HasCode(Sink, TEXT("var_undefined")), DescribeDiags(Sink)));
		ASSERT_THAT(IsTrue(HasCode(Sink, TEXT("unknown_field")), DescribeDiags(Sink)));
	}

	/** 只认 schemaVersion 5：旧版本报 schema_old（不做迁移），不再按旧语义跑。 */
	TEST_METHOD(Parse_RejectsOldSchema)
	{
		using namespace GameTestScriptFlowTestLocal;
		FGameTestDiagSink Sink;
		ParseText(TEXT(R"({"kind":"gametest-scenario","schemaVersion":2,"name":"t","steps":[
			{"action":"mark","text":"1"},{"action":"mark","text":"2"}]})"), Sink);
		ASSERT_THAT(IsTrue(HasCode(Sink, TEXT("schema_old")), DescribeDiags(Sink)));
	}

	/** v4 的对象写法：objects 里的引用、部件写法、条件里的对象判断都能过加载检查；旧选择器报 selector_removed。 */
	TEST_METHOD(Parse_AcceptsRefsAndRejectsSelectors)
	{
		using namespace GameTestScriptFlowTestLocal;
		FGameTestDiagSink Good;
		ParseText(TEXT(R"({"kind":"gametest-scenario","schemaVersion":5,"name":"t",
			"objects":{"shop":{"class":"WBP_Shop","ident":[{"prop":"@.ShopId","op":"eq","value":1001}]}},
			"steps":[
			{"id":"a","action":"click","target":{"obj":"shop","part":"BtnBuy"}},
			{"id":"b","action":"wait","until":"cond","cond":{"obj":"shop","prop":"@:onScreen","op":"is","value":false}},
			{"id":"c","action":"obj.close","obj":"shop","how":[{"by":"remove"}]}]})"), Good);
		ASSERT_THAT(IsFalse(Good.HasErrors(), DescribeDiags(Good)));

		FGameTestDiagSink Bad;
		ParseText(TEXT(R"({"kind":"gametest-scenario","schemaVersion":5,"name":"t","steps":[
			{"id":"a","action":"click","target":"WBP_Shop:Root/BtnBuy"}]})"), Bad);
		ASSERT_THAT(IsTrue(HasCode(Bad, TEXT("selector_removed")), DescribeDiags(Bad)));
	}

	/** if 按变量走分支；没走的分支不记录。 */
	TEST_METHOD(Run_IfPicksBranchByVar)
	{
		using namespace GameTestScriptFlowTestLocal;
		FGameTestDiagSink Sink;
		const TSharedRef<FGameTestScript> Script = ParseText(TEXT(R"({"kind":"gametest-scenario","schemaVersion":5,"name":"t",
			"vars":{"n":2},
			"steps":[{"id":"pick","action":"if","cond":{"expr":"$n > 1","settleMs":0},
				"then":[{"id":"big","action":"mark","text":"big"}],
				"else":[{"id":"small","action":"mark","text":"small"}]}]})"), Sink);
		ASSERT_THAT(IsFalse(Sink.HasErrors(), DescribeDiags(Sink)));
		FGameTestRunner Runner;
		FString Error;
		const FGameTestReport& Report = RunToEnd(Runner, Script, Error);
		ASSERT_THAT(IsTrue(Error.IsEmpty(), Error));
		ASSERT_THAT(AreEqual(1, CountById(Report, TEXT("big"), EGameTestStepStatus::Succeeded), DescribeReport(Report)));
		ASSERT_THAT(AreEqual(0, CountById(Report, TEXT("small"), EGameTestStepStatus::Succeeded), DescribeReport(Report)));
		ASSERT_THAT(IsTrue(Report.bSucceeded, DescribeReport(Report)));
	}

	/** loop times 跑够轮数，每轮的记录带 iter；break 带条件时提前出循环。 */
	TEST_METHOD(Run_LoopTimesAndBreak)
	{
		using namespace GameTestScriptFlowTestLocal;
		FGameTestDiagSink Sink;
		const TSharedRef<FGameTestScript> Script = ParseText(TEXT(R"({"kind":"gametest-scenario","schemaVersion":5,"name":"t","steps":[
			{"id":"three","action":"loop","times":3,"do":[{"id":"tick","action":"mark","text":"轮"}]},
			{"id":"early","action":"loop","times":5,"do":[
				{"id":"once","action":"mark","text":"x"},
				{"action":"break","cond":{"expr":"$index == 1","settleMs":0}}]}]})"), Sink);
		ASSERT_THAT(IsFalse(Sink.HasErrors(), DescribeDiags(Sink)));
		FGameTestRunner Runner;
		FString Error;
		const FGameTestReport& Report = RunToEnd(Runner, Script, Error);
		ASSERT_THAT(AreEqual(3, CountById(Report, TEXT("tick"), EGameTestStepStatus::Succeeded), DescribeReport(Report)));
		ASSERT_THAT(AreEqual(2, CountById(Report, TEXT("once"), EGameTestStepStatus::Succeeded), DescribeReport(Report)));
		ASSERT_THAT(IsTrue(Report.bSucceeded, DescribeReport(Report)));
	}

	/** 块上 onFail: continue 吞掉块里的失败，后面的步骤照跑。 */
	TEST_METHOD(Run_BlockOnFailContinue)
	{
		using namespace GameTestScriptFlowTestLocal;
		FGameTestDiagSink Sink;
		const TSharedRef<FGameTestScript> Script = ParseText(TEXT(R"({"kind":"gametest-scenario","schemaVersion":5,"name":"t","steps":[
			{"id":"g","action":"group","name":"会失败的段","onFail":"continue","do":[
				{"id":"bad","action":"fail","message":"故意失败"},
				{"id":"never","action":"mark","text":"不该跑"}]},
			{"id":"after","action":"mark","text":"照跑"}]})"), Sink);
		ASSERT_THAT(IsFalse(Sink.HasErrors(), DescribeDiags(Sink)));
		FGameTestRunner Runner;
		FString Error;
		const FGameTestReport& Report = RunToEnd(Runner, Script, Error);
		ASSERT_THAT(AreEqual(0, CountById(Report, TEXT("never"), EGameTestStepStatus::Succeeded), DescribeReport(Report)));
		ASSERT_THAT(AreEqual(1, CountById(Report, TEXT("after"), EGameTestStepStatus::Succeeded), DescribeReport(Report)));
	}

	/** set 写变量，assert expr 读它；断言不成立时 run 失败。 */
	TEST_METHOD(Run_SetAndAssertExpr)
	{
		using namespace GameTestScriptFlowTestLocal;
		FGameTestDiagSink Sink;
		const TSharedRef<FGameTestScript> Script = ParseText(TEXT(R"({"kind":"gametest-scenario","schemaVersion":5,"name":"t","steps":[
			{"id":"setX","action":"set","var":"x","value":"=2 + 3"},
			{"id":"ok","action":"assert","expr":"$x == 5"},
			{"id":"bad","action":"assert","expr":"$x == 6"}]})"), Sink);
		ASSERT_THAT(IsFalse(Sink.HasErrors(), DescribeDiags(Sink)));
		FGameTestRunner Runner;
		FString Error;
		const FGameTestReport& Report = RunToEnd(Runner, Script, Error);
		ASSERT_THAT(AreEqual(1, CountById(Report, TEXT("ok"), EGameTestStepStatus::Succeeded), DescribeReport(Report)));
		ASSERT_THAT(AreEqual(1, CountById(Report, TEXT("bad"), EGameTestStepStatus::Failed), DescribeReport(Report)));
		ASSERT_THAT(IsFalse(Report.bSucceeded));
	}

	/** disabled 的步骤不跑，报告记 Disabled。 */
	TEST_METHOD(Run_DisabledStepIsRecorded)
	{
		using namespace GameTestScriptFlowTestLocal;
		FGameTestDiagSink Sink;
		const TSharedRef<FGameTestScript> Script = ParseText(TEXT(R"({"kind":"gametest-scenario","schemaVersion":5,"name":"t","steps":[
			{"id":"off","action":"fail","message":"不该跑","disabled":true},
			{"id":"on","action":"mark","text":"跑"}]})"), Sink);
		ASSERT_THAT(IsFalse(Sink.HasErrors(), DescribeDiags(Sink)));
		FGameTestRunner Runner;
		FString Error;
		const FGameTestReport& Report = RunToEnd(Runner, Script, Error);
		ASSERT_THAT(AreEqual(1, CountById(Report, TEXT("off"), EGameTestStepStatus::Disabled), DescribeReport(Report)));
		ASSERT_THAT(AreEqual(1, CountById(Report, TEXT("on"), EGameTestStepStatus::Succeeded), DescribeReport(Report)));
	}
};

#endif
