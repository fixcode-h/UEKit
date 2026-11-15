// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestRunStore.h"

#include "Core/GameTestJson.h"

#include "Dom/JsonObject.h"

GameTestRunStore::GameTestRunStore(int32 InCapacity)
{
	SetCapacity(InCapacity);
}

void GameTestRunStore::SetCapacity(int32 InCapacity)
{
	// 上限至少为 1：留 0 份等于"历史永远为空"，那是静默失效而不是配置。
	Capacity = FMath::Max(1, InCapacity);
	while (Reports.Num() > Capacity)
	{
		Reports.RemoveAt(0, /*Count=*/1, EAllowShrinking::No);
	}
}

void GameTestRunStore::Add(const TSharedPtr<FJsonObject>& InReportJson)
{
	if (!InReportJson.IsValid())
	{
		return;
	}

	// 头是最旧的，尾是最新的：淘汰就从头部摘。
	Reports.Add(InReportJson);
	while (Reports.Num() > Capacity)
	{
		Reports.RemoveAt(0, /*Count=*/1, EAllowShrinking::No);
	}
}

TArray<TSharedPtr<FJsonObject>> GameTestRunStore::ListSummaries() const
{
	TArray<TSharedPtr<FJsonObject>> Summaries;
	Summaries.Reserve(Reports.Num());

	// 倒序：调用方要的是"最近一次在最前"。
	for (int32 Index = Reports.Num() - 1; Index >= 0; --Index)
	{
		if (Reports[Index].IsValid())
		{
			Summaries.Add(MakeSummary(*Reports[Index]));
		}
	}

	return Summaries;
}

TSharedPtr<FJsonObject> GameTestRunStore::Find(const FString& InRunId) const
{
	if (InRunId.IsEmpty())
	{
		return nullptr;
	}

	for (const TSharedPtr<FJsonObject>& Report : Reports)
	{
		FString ReportRunId;
		if (Report.IsValid() && GameTestJson::TryGetString(*Report, TEXT("runId"), ReportRunId) && ReportRunId == InRunId)
		{
			return Report;
		}
	}

	return nullptr;
}

void GameTestRunStore::Reset()
{
	Reports.Reset();
}

TSharedPtr<FJsonObject> GameTestRunStore::MakeSummary(const FJsonObject& InReportJson)
{
	const TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();

	FString Text;
	if (GameTestJson::TryGetString(InReportJson, TEXT("runId"), Text))
	{
		Summary->SetStringField(TEXT("runId"), Text);
	}
	if (GameTestJson::TryGetString(InReportJson, TEXT("name"), Text))
	{
		Summary->SetStringField(TEXT("name"), Text);
	}
	if (GameTestJson::TryGetString(InReportJson, TEXT("startedAtUtc"), Text))
	{
		Summary->SetStringField(TEXT("startedAtUtc"), Text);
	}
	if (GameTestJson::TryGetString(InReportJson, TEXT("finishedAtUtc"), Text))
	{
		Summary->SetStringField(TEXT("finishedAtUtc"), Text);
	}

	bool bSucceeded = false;
	if (GameTestJson::TryGetBool(InReportJson, TEXT("succeeded"), bSucceeded))
	{
		Summary->SetBoolField(TEXT("succeeded"), bSucceeded);
	}

	double Number = 0.0;
	if (GameTestJson::TryGetNumber(InReportJson, TEXT("revision"), Number))
	{
		Summary->SetNumberField(TEXT("revision"), Number);
	}
	if (GameTestJson::TryGetNumber(InReportJson, TEXT("durationMs"), Number))
	{
		Summary->SetNumberField(TEXT("durationMs"), Number);
	}
	if (GameTestJson::TryGetNumber(InReportJson, TEXT("total"), Number))
	{
		Summary->SetNumberField(TEXT("total"), Number);
	}
	if (GameTestJson::TryGetNumber(InReportJson, TEXT("failed"), Number))
	{
		Summary->SetNumberField(TEXT("failed"), Number);
	}

	return Summary;
}
