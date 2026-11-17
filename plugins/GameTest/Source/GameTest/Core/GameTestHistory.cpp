// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestHistory.h"

#include "Core/GameTestJson.h"
#include "Core/GameTestTypes.h"
#include "GameTest.h"
#include "GameTestCompat.h"
#include "Shot/GameTestShot.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	/** 历史里失败原因的长度：列表的提示里放得下一两句就够，完整的看报告。 */
	constexpr int32 HistoryMaxMessageChars = 200;
}

FString GameTestHistory::FilePath()
{
	return FPaths::Combine(GameTestShot::GetOutputRootDir(), TEXT("history.json"));
}

void GameTestHistory::Append(FJsonObject& InOutAll, const FString& InScenario, const TSharedPtr<FJsonObject>& InEntry)
{
	if (InScenario.IsEmpty() || !InEntry.IsValid())
	{
		return;
	}

	TSharedPtr<FJsonObject> Scenarios = GameTestJson::TryGetObject(InOutAll, TEXT("scenarios"));
	if (!Scenarios.IsValid())
	{
		Scenarios = MakeShared<FJsonObject>();
		InOutAll.SetObjectField(TEXT("scenarios"), Scenarios);
	}

	TArray<TSharedPtr<FJsonValue>> List;
	if (const TArray<TSharedPtr<FJsonValue>>* Old = GameTestJson::TryGetArray(*Scenarios, *InScenario))
	{
		List = *Old;
	}
	List.Insert(MakeShared<FJsonValueObject>(InEntry), 0);
	if (List.Num() > Keep)
	{
		List.SetNum(Keep);
	}
	Scenarios->SetArrayField(InScenario, List);
}

TSharedPtr<FJsonObject> GameTestHistory::MakeEntry(const FGameTestReport& InReport, const FString& InOutputDirRelative)
{
	const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("runId"), InReport.RunId);
	Entry->SetBoolField(TEXT("succeeded"), InReport.bSucceeded);
	Entry->SetStringField(TEXT("startedAtUtc"), InReport.StartedAtUtc);
	Entry->SetNumberField(TEXT("durationMs"), FMath::RoundToDouble(InReport.DurationMs));
	Entry->SetNumberField(TEXT("total"), InReport.Steps.Num());
	Entry->SetNumberField(TEXT("failed"), InReport.CountByStatus(EGameTestStepStatus::Failed));
	if (InReport.Revision > 0)
	{
		Entry->SetNumberField(TEXT("revision"), InReport.Revision);
	}
	if (!InOutputDirRelative.IsEmpty())
	{
		Entry->SetStringField(TEXT("outputDir"), InOutputDirRelative);
	}

	for (const FGameTestStepRecord& Record : InReport.Steps)
	{
		if (Record.Status == EGameTestStepStatus::Failed)
		{
			// 第几步（从 1 数，和面板、报告里说的一致）和原因
			// 前置步骤失败记成第 0 步：场景本身一步都没跑
			Entry->SetNumberField(TEXT("failedStep"), Record.Step.bSetup ? 0 : Record.Step.Index + 1);
			Entry->SetStringField(TEXT("message"), Record.Message.Left(HistoryMaxMessageChars));
			break;
		}
	}
	return Entry;
}

TSharedPtr<FJsonObject> GameTestHistory::Load()
{
	FString Error;
	TSharedPtr<FJsonObject> All;
	if (FPaths::FileExists(FilePath()))
	{
		All = GameTestJson::ParseObjectFromFile(FilePath(), Error);
		if (!All.IsValid())
		{
			UE_LOG(LogGameTest, Warning, TEXT("history.json 读不了（%s），当作没有历史"), *Error);
		}
	}
	if (!All.IsValid())
	{
		All = MakeShared<FJsonObject>();
	}
	if (!GameTestJson::TryGetObject(*All, TEXT("scenarios")).IsValid())
	{
		All->SetObjectField(TEXT("scenarios"), MakeShared<FJsonObject>());
	}
	return All;
}

void GameTestHistory::Record(const FGameTestReport& InReport, const FString& InOutputDirRelative)
{
	if (InReport.Scenario.IsEmpty())
	{
		return;
	}

	const TSharedPtr<FJsonObject> All = Load();
	Append(*All, InReport.Scenario, MakeEntry(InReport, InOutputDirRelative));

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath()), /*Tree=*/true);
	if (!FFileHelper::SaveStringToFile(GameTestJson::ToPrettyString(All), *FilePath(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogGameTest, Warning, TEXT("运行历史写不进 %s（这次 run 的结论不受影响）"), *FilePath());
	}
}

TSharedPtr<FJsonObject> GameTestHistory::Compact(const FJsonObject& InAll)
{
	const TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject> Scenarios = GameTestJson::TryGetObject(InAll, TEXT("scenarios"));
	if (!Scenarios.IsValid())
	{
		return Out;
	}

	for (const auto& Pair : Scenarios->Values)
	{
		const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
		if (!Pair.Value.IsValid() || !Pair.Value->TryGetArray(List) || List == nullptr)
		{
			continue;
		}
		FString Marks;
		for (const TSharedPtr<FJsonValue>& Item : *List)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			bool bSucceeded = false;
			if (Item.IsValid() && Item->TryGetObject(Entry) && Entry != nullptr)
			{
				(*Entry)->TryGetBoolField(TEXT("succeeded"), bSucceeded);
			}
			Marks.AppendChar(bSucceeded ? TEXT('p') : TEXT('f'));
		}
		Out->SetStringField(GAMETEST_JSON_KEY(Pair.Key), Marks);
	}
	return Out;
}
