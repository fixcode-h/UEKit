// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestBatch.h"

#include "Core/GameTestTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	/** 批量结果里每项消息的长度：几十个场景的批量也要能装进一次推送。 */
	constexpr int32 BatchMaxMessageChars = 200;
}

void FGameTestBatch::Start(const FString& InId, const FString& InLabel, const TArray<FString>& InScenarios)
{
	Id = InId;
	Label = InLabel;
	Items.Reset();
	for (const FString& Scenario : InScenarios)
	{
		FGameTestBatchItem Item;
		Item.Scenario = Scenario;
		Items.Add(MoveTemp(Item));
	}
	Current = INDEX_NONE;
	bActive = Items.Num() > 0;
	++Serial;
}

int32 FGameTestBatch::TakeNext()
{
	if (!bActive)
	{
		return INDEX_NONE;
	}
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		if (Items[Index].Status == TEXT("pending"))
		{
			Items[Index].Status = TEXT("running");
			Current = Index;
			++Serial;
			return Index;
		}
	}
	return INDEX_NONE;
}

void FGameTestBatch::MarkStarted(int32 InIndex, const FString& InRunId, const FString& InName)
{
	if (!Items.IsValidIndex(InIndex))
	{
		return;
	}
	FGameTestBatchItem& Item = Items[InIndex];
	if (Item.RunId.IsEmpty())
	{
		Item.RunId = InRunId;
	}
	if (Item.Name.IsEmpty())
	{
		Item.Name = InName;
	}
	++Serial;
}

void FGameTestBatch::MarkError(int32 InIndex, const FString& InMessage)
{
	if (!Items.IsValidIndex(InIndex))
	{
		return;
	}
	Items[InIndex].Status = TEXT("error");
	Items[InIndex].Message = InMessage.Left(BatchMaxMessageChars);
	++Serial;
}

void FGameTestBatch::OnRunFinished(const FGameTestReport& InReport)
{
	// 起 run 的那一下可能就跑完了（第一步就失败）：那时还没记 runId，所以按「正在跑的那一项」对，不按 runId
	if (!Items.IsValidIndex(Current) || Items[Current].Status != TEXT("running"))
	{
		return;
	}
	FGameTestBatchItem& Item = Items[Current];
	Item.RunId = InReport.RunId;
	Item.Name = InReport.Name;
	Item.DurationMs = InReport.DurationMs;
	Item.Status = InReport.bSucceeded ? TEXT("passed") : TEXT("failed");
	for (const FGameTestStepRecord& Record : InReport.Steps)
	{
		if (Record.Status == EGameTestStepStatus::Failed && !Record.bWarn)
		{
			Item.FailedStep = Record.Step.bSetup ? 0 : Record.Step.Index + 1;
			Item.Message = Record.Message.Left(BatchMaxMessageChars);
			break;
		}
	}
	++Serial;
}

void FGameTestBatch::Cancel()
{
	if (!bActive)
	{
		return;
	}
	for (FGameTestBatchItem& Item : Items)
	{
		if (Item.Status == TEXT("pending"))
		{
			Item.Status = TEXT("skipped");
			Item.Message = TEXT("批量运行被取消，没有跑");
		}
	}
	bActive = false;
	++Serial;
}

bool FGameTestBatch::FinishIfDone()
{
	if (!bActive)
	{
		return false;
	}
	for (const FGameTestBatchItem& Item : Items)
	{
		if (Item.Status == TEXT("pending") || Item.Status == TEXT("running"))
		{
			return false;
		}
	}
	bActive = false;
	++Serial;
	return true;
}

void FGameTestBatch::Clear()
{
	if (Items.Num() == 0 && !bActive)
	{
		return;
	}
	Items.Reset();
	Current = INDEX_NONE;
	bActive = false;
	++Serial;
}

int32 FGameTestBatch::Count(const TCHAR* InStatus) const
{
	int32 Count = 0;
	for (const FGameTestBatchItem& Item : Items)
	{
		if (Item.Status == InStatus)
		{
			++Count;
		}
	}
	return Count;
}

TSharedPtr<FJsonObject> FGameTestBatch::ToJson() const
{
	const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("id"), Id);
	Json->SetStringField(TEXT("label"), Label);
	Json->SetBoolField(TEXT("active"), bActive);
	Json->SetNumberField(TEXT("total"), Items.Num());
	Json->SetNumberField(TEXT("current"), Current);
	Json->SetNumberField(TEXT("passed"), Count(TEXT("passed")));
	Json->SetNumberField(TEXT("failed"), Count(TEXT("failed")) + Count(TEXT("error")));
	Json->SetNumberField(TEXT("skipped"), Count(TEXT("skipped")));

	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FGameTestBatchItem& Item : Items)
	{
		const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("scenario"), Item.Scenario);
		if (!Item.Name.IsEmpty())
		{
			Entry->SetStringField(TEXT("name"), Item.Name);
		}
		Entry->SetStringField(TEXT("status"), Item.Status);
		if (!Item.RunId.IsEmpty())
		{
			Entry->SetStringField(TEXT("runId"), Item.RunId);
		}
		if (!Item.Message.IsEmpty())
		{
			Entry->SetStringField(TEXT("message"), Item.Message);
		}
		if (Item.FailedStep > 0)
		{
			Entry->SetNumberField(TEXT("failedStep"), Item.FailedStep);
		}
		if (Item.DurationMs > 0.0)
		{
			Entry->SetNumberField(TEXT("durationMs"), FMath::RoundToDouble(Item.DurationMs));
		}
		Values.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Json->SetArrayField(TEXT("items"), Values);
	return Json;
}
