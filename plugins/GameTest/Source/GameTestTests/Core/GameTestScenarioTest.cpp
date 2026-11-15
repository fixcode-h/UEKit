// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/GameTestJson.h"
#include "Core/GameTestScenario.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_AUTOMATION_WORKER

namespace
{
	TSharedPtr<FJsonObject> ParseScenario(const FString& InText)
	{
		FString Error;
		return GameTestJson::ParseObjectFromText(InText, Error);
	}
}

TEST_CLASS_WITH_FLAGS(GameTestScenarioTest, "GameTest.Scenario",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	TEST_METHOD(Validate_AcceptsValidScenario)
	{
		TArray<TSharedPtr<FJsonObject>> Steps;
		TArray<FString> Errors;
		ASSERT_THAT(IsTrue(GameTestScenario::CollectSteps(
			ParseScenario(TEXT(R"({"kind":"gametest-scenario","schemaVersion":1,"name":"smoke","steps":[{"action":"ping"}]})")),
			Steps, Errors), FString::Join(Errors, TEXT("；"))));
		ASSERT_THAT(IsTrue(Steps.Num() == 1, TEXT("应收到一条步骤")));
	}

	TEST_METHOD(Validate_RejectsWrongKind)
	{
		TArray<TSharedPtr<FJsonObject>> Steps;
		TArray<FString> Errors;
		ASSERT_THAT(IsFalse(GameTestScenario::CollectSteps(
			ParseScenario(TEXT(R"({"kind":"something-else","schemaVersion":1,"name":"x","steps":[{"action":"ping"}]})")),
			Steps, Errors)));
		ASSERT_THAT(IsTrue(FString::Join(Errors, TEXT("；")).Contains(TEXT("gametest-scenario")), *FString::Join(Errors, TEXT("；"))));
	}

	TEST_METHOD(Validate_RejectsMissingSchemaVersion)
	{
		TArray<TSharedPtr<FJsonObject>> Steps;
		TArray<FString> Errors;
		ASSERT_THAT(IsFalse(GameTestScenario::CollectSteps(
			ParseScenario(TEXT(R"({"kind":"gametest-scenario","name":"x","steps":[{"action":"ping"}]})")),
			Steps, Errors)));
		ASSERT_THAT(IsTrue(FString::Join(Errors, TEXT("；")).Contains(TEXT("schemaVersion")), *FString::Join(Errors, TEXT("；"))));
	}

	TEST_METHOD(Validate_RejectsVersionAboveSupported)
	{
		TArray<TSharedPtr<FJsonObject>> Steps;
		TArray<FString> Errors;
		ASSERT_THAT(IsFalse(GameTestScenario::CollectSteps(
			ParseScenario(TEXT(R"({"kind":"gametest-scenario","schemaVersion":99,"name":"x","steps":[{"action":"ping"}]})")),
			Steps, Errors)));
		ASSERT_THAT(IsTrue(Steps.Num() == 0, TEXT("被拒时不应产出任何步骤（一步都不能执行）")));
		ASSERT_THAT(IsTrue(FString::Join(Errors, TEXT("；")).Contains(TEXT("99")), *FString::Join(Errors, TEXT("；"))));
	}

	TEST_METHOD(Validate_RejectsMissingName)
	{
		TArray<TSharedPtr<FJsonObject>> Steps;
		TArray<FString> Errors;
		ASSERT_THAT(IsFalse(GameTestScenario::CollectSteps(
			ParseScenario(TEXT(R"({"kind":"gametest-scenario","schemaVersion":1,"steps":[{"action":"ping"}]})")),
			Steps, Errors)));
	}

	TEST_METHOD(Validate_RejectsEmptySteps)
	{
		TArray<TSharedPtr<FJsonObject>> Steps;
		TArray<FString> Errors;
		ASSERT_THAT(IsFalse(GameTestScenario::CollectSteps(
			ParseScenario(TEXT(R"({"kind":"gametest-scenario","schemaVersion":1,"name":"x","steps":[]})")),
			Steps, Errors)));
	}

	TEST_METHOD(Validate_RejectsNonObjectStep)
	{
		TArray<TSharedPtr<FJsonObject>> Steps;
		TArray<FString> Errors;
		ASSERT_THAT(IsFalse(GameTestScenario::CollectSteps(
			ParseScenario(TEXT(R"({"kind":"gametest-scenario","schemaVersion":1,"name":"x","steps":[123]})")),
			Steps, Errors)));
	}

	TEST_METHOD(GetName_FallsBackWhenMissing)
	{
		// GetName 只在通过校验的场景上调用；这里直接验"没有 name 就用文件名"这条兜底。
		ASSERT_THAT(IsTrue(GameTestScenario::GetName(ParseScenario(TEXT(R"({"kind":"gametest-scenario"})")), TEXT("fallback")) == TEXT("fallback"),
			TEXT("没有 name 时应退回备选名")));
	}

	TEST_METHOD(GetRevision_ReadsValueOrDefaultsToZero)
	{
		ASSERT_THAT(AreEqual(7, GameTestScenario::GetRevision(
			ParseScenario(TEXT(R"({"kind":"gametest-scenario","revision":7})"))), TEXT("写了 revision 要读出来")));
		ASSERT_THAT(AreEqual(0, GameTestScenario::GetRevision(
			ParseScenario(TEXT(R"({"kind":"gametest-scenario"})"))), TEXT("没写就是 0")));
		// 负数没有任何语义，当成"没写"更安全，别把一个无意义的值带进报告。
		ASSERT_THAT(AreEqual(0, GameTestScenario::GetRevision(
			ParseScenario(TEXT(R"({"kind":"gametest-scenario","revision":-3})"))), TEXT("负数按 0 处理")));
	}

	TEST_METHOD(GetTags_ReadsStringsAndDegradesGracefully)
	{
		const TArray<FString> Tags = GameTestScenario::GetTags(
			ParseScenario(TEXT(R"({"kind":"gametest-scenario","tags":["smoke"," ui "]})")));
		ASSERT_THAT(AreEqual(2, Tags.Num(), TEXT("两个标签都要读到")));
		ASSERT_THAT(IsTrue(Tags.Contains(TEXT("smoke")), TEXT("原样保留")));
		ASSERT_THAT(IsTrue(Tags.Contains(TEXT("ui")), TEXT("元素要 trim")));

		ASSERT_THAT(AreEqual(0, GameTestScenario::GetTags(ParseScenario(TEXT(R"({"kind":"gametest-scenario"})"))).Num(),
			TEXT("没写 tags 返回空数组")));
		// tags 是标注信息：类型写错不该让整份场景不可用，退回空数组即可。
		ASSERT_THAT(AreEqual(0, GameTestScenario::GetTags(ParseScenario(TEXT(R"({"kind":"gametest-scenario","tags":"smoke"})"))).Num(),
			TEXT("tags 不是数组时返回空数组")));
	}

	TEST_METHOD(ScanDirectory_ReturnsSortedScenariosWithErrorsKept)
	{
		// 真扫一个临时目录：这条契约的价值恰恰在"读真实文件"，桩掉文件系统就测不到它。
		const FString RootDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("GameTestScanTest"));
		IFileManager::Get().DeleteDirectory(*RootDir, /*RequireExists=*/false, /*Tree=*/true);
		IFileManager::Get().MakeDirectory(*FPaths::Combine(RootDir, TEXT("pie")), /*Tree=*/true);

		const auto WriteScenario = [&RootDir](const FString& InRelative, const FString& InText)
		{
			FFileHelper::SaveStringToFile(InText, *FPaths::Combine(RootDir, InRelative), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		};

		WriteScenario(TEXT("pie/b-second.json"),
			TEXT(R"({"kind":"gametest-scenario","schemaVersion":1,"name":"b","revision":2,"tags":["slow"],"steps":[{"action":"ping"}]})"));
		WriteScenario(TEXT("a-first.json"),
			TEXT(R"({"kind":"gametest-scenario","schemaVersion":1,"name":"a","steps":[{"action":"ping"},{"action":"ping"}]})"));
		// 坏场景：缺 steps。它必须仍然出现在清单里，并带上原因。
		WriteScenario(TEXT("broken.json"),
			TEXT(R"({"kind":"gametest-scenario","schemaVersion":1,"name":"broken"})"));

		const TArray<FGameTestScenarioInfo> Found = GameTestScenario::ScanDirectory(RootDir);

		ASSERT_THAT(AreEqual(3, Found.Num(), TEXT("三个文件都要出现")));
		// 顺序稳定：这样 AI 比较两次列举结果才有意义。
		ASSERT_THAT(IsTrue(Found[0].RelativePath == TEXT("a-first.json"), *Found[0].RelativePath));
		ASSERT_THAT(IsTrue(Found[1].RelativePath == TEXT("broken.json"), *Found[1].RelativePath));
		ASSERT_THAT(IsTrue(Found[2].RelativePath == TEXT("pie/b-second.json"), *Found[2].RelativePath));

		ASSERT_THAT(AreEqual(2, Found[0].StepCount, TEXT("步数要数出来")));
		ASSERT_THAT(AreEqual(2, Found[2].Revision, TEXT("revision 要读出来")));
		ASSERT_THAT(IsTrue(Found[2].Tags.Contains(TEXT("slow")), TEXT("tags 要读出来")));

		ASSERT_THAT(IsTrue(!Found[1].ParseError.IsEmpty(), TEXT("坏场景要带原因，不能被静默跳过")));
		ASSERT_THAT(IsTrue(Found[1].Name == TEXT("broken"), TEXT("坏场景仍尽力取出 name")));

		IFileManager::Get().DeleteDirectory(*RootDir, /*RequireExists=*/false, /*Tree=*/true);
	}
};

#endif	// WITH_AUTOMATION_WORKER
