// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestTypes.h"

#include "Dom/JsonObject.h"

const TCHAR* GameTestStatus::ToString(EGameTestStepStatus InStatus)
{
	switch (InStatus)
	{
	case EGameTestStepStatus::Succeeded: return TEXT("succeeded");
	case EGameTestStepStatus::Failed:    return TEXT("failed");
	case EGameTestStepStatus::Skipped:   return TEXT("skipped");
	case EGameTestStepStatus::Pending:   return TEXT("pending");
	case EGameTestStepStatus::Already:   return TEXT("already");
	case EGameTestStepStatus::Disabled:  return TEXT("disabled");
	}
	return TEXT("unknown");
}

TSharedPtr<FJsonObject> FGameTestStepRecord::ToJson() const
{
	const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("index"), Step.Index);
	if (Step.bSetup)
	{
		// 前置步骤的 index 是它在前置步骤里的序号：面板按这个字段把它们和场景步骤分开显示。
		Json->SetBoolField(TEXT("setup"), true);
	}
	Json->SetStringField(TEXT("action"), Step.Action.ToString());
	Json->SetStringField(TEXT("status"), GameTestStatus::ToString(Status));
	// 1.9.0 脚本：id / path 认步骤（块里的步骤没有「第几步」），块的记录标 block，循环里的标轮次。
	if (!Step.Id.IsEmpty())
	{
		Json->SetStringField(TEXT("id"), Step.Id);
	}
	if (!Step.Path.IsEmpty())
	{
		Json->SetStringField(TEXT("path"), Step.Path);
	}
	if (bBlock)
	{
		Json->SetBoolField(TEXT("block"), true);
	}
	if (Iter >= 0)
	{
		Json->SetNumberField(TEXT("iter"), Iter);
	}
	Json->SetStringField(TEXT("message"), Message);

	if (!Step.Target.IsEmpty())
	{
		Json->SetStringField(TEXT("target"), Step.Target);
	}
	if (!Step.Note.IsEmpty())
	{
		Json->SetStringField(TEXT("note"), Step.Note);
	}
	if (Payload.IsValid())
	{
		Json->SetObjectField(TEXT("payload"), Payload);
	}

	Json->SetNumberField(TEXT("durationMs"), (FinishedAt - StartedAt) * 1000.0);
	return Json;
}

int32 FGameTestReport::CountByStatus(EGameTestStepStatus InStatus) const
{
	int32 Count = 0;
	for (const FGameTestStepRecord& Record : Steps)
	{
		if (Record.Status == InStatus)
		{
			++Count;
		}
	}
	return Count;
}

TSharedPtr<FJsonObject> FGameTestReport::ToJson() const
{
	const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("runId"), RunId);
	Json->SetStringField(TEXT("name"), Name);
	if (!Scenario.IsEmpty())
	{
		Json->SetStringField(TEXT("scenario"), Scenario);
	}
	Json->SetNumberField(TEXT("revision"), Revision);
	Json->SetBoolField(TEXT("succeeded"), bSucceeded);
	Json->SetStringField(TEXT("startedAtUtc"), StartedAtUtc);
	Json->SetStringField(TEXT("finishedAtUtc"), FinishedAtUtc);
	Json->SetNumberField(TEXT("durationMs"), DurationMs);
	Json->SetNumberField(TEXT("total"), Steps.Num());
	if (Guards.IsValid() && Guards->Values.Num() > 0)
	{
		Json->SetObjectField(TEXT("guards"), Guards);
	}
	Json->SetNumberField(TEXT("failed"), CountByStatus(EGameTestStepStatus::Failed));
	// 已达成跳过的步数（1.6.0）：算通过，单独计数，面板结果条上写「N 步已达成跳过」
	const int32 Already = CountByStatus(EGameTestStepStatus::Already);
	if (Already > 0)
	{
		Json->SetNumberField(TEXT("already"), Already);
	}

	TArray<TSharedPtr<FJsonValue>> StepValues;
	StepValues.Reserve(Steps.Num());
	for (const FGameTestStepRecord& Record : Steps)
	{
		StepValues.Add(MakeShared<FJsonValueObject>(Record.ToJson()));
	}
	Json->SetArrayField(TEXT("steps"), StepValues);

	return Json;
}

FString GameTestRun::MakeRunId(const FString& InName, int32 InSequence)
{
	FString Slug;
	Slug.Reserve(InName.Len() + 8);

	bool bPendingSeparator = false;
	for (const TCHAR Character : InName)
	{
		// 只放行 ASCII 字母与数字；其余（中文、空白、标点）一律折成连字符。
		// 折成 '-' 而不是删掉，是为了"载具 开火"与"载具开火"不会撞成同一个 id。
		if ((Character >= TEXT('a') && Character <= TEXT('z'))
			|| (Character >= TEXT('0') && Character <= TEXT('9')))
		{
			if (bPendingSeparator && !Slug.IsEmpty())
			{
				Slug.AppendChar(TEXT('-'));
			}
			bPendingSeparator = false;
			Slug.AppendChar(Character);
		}
		else if (Character >= TEXT('A') && Character <= TEXT('Z'))
		{
			if (bPendingSeparator && !Slug.IsEmpty())
			{
				Slug.AppendChar(TEXT('-'));
			}
			bPendingSeparator = false;
			Slug.AppendChar(FChar::ToLower(Character));
		}
		else if (!Slug.IsEmpty())
		{
			// 连续的非字母数字只记一个待补的连字符：首尾不留、中间不重复。
			bPendingSeparator = true;
		}
	}

	if (Slug.IsEmpty())
	{
		Slug = TEXT("run");
	}

	return FString::Printf(TEXT("%s-%d"), *Slug, InSequence);
}
