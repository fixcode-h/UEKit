// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestEvidence.h"

#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestLogTap.h"
#include "Core/GameTestTypes.h"
#include "Core/GameTestWorld.h"
#include "Observe/GameTestSnapshot.h"
#include "Observe/GameTestWidgetNode.h"
#include "Shot/GameTestShot.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	/** 「之后」的日志最多等多久：Stop 模式下失败后马上收尾，等不来 50 行也要把文件写完整。 */
	constexpr double EvidenceLogWaitSeconds = 3.0;

	/** 控件快照的节点上限：够看清失败时屏上有什么，又不至于整棵树几 MB。 */
	constexpr int32 EvidenceMaxNodes = 3000;

	/** 选择器候选的说明截断：候选清单可能很长，证据里放够判断的那一段。 */
	constexpr int32 EvidenceMaxSelectorChars = 2000;
}

FGameTestEvidence::FGameTestEvidence(FGameTestLogTap& InLogTap)
	: LogTap(InLogTap)
{
}

void FGameTestEvidence::BeginRun()
{
	CapturedThisRun = 0;
}

void FGameTestEvidence::Capture(FGameTestStepRecord& InOutRecord, const FString& InRunDir, TMap<FString, TWeakObjectPtr<UWidget>>* InHandles)
{
	if (InRunDir.IsEmpty() || CapturedThisRun >= MaxPerRun)
	{
		return;
	}
	++CapturedThisRun;

	const FString Dir = FPaths::Combine(InRunDir, TEXT("failures"), FString::Printf(TEXT("step-%02d"), InOutRecord.Step.Index + 1));
	IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);

	const TSharedPtr<FJsonObject> Evidence = MakeShared<FJsonObject>();
	Evidence->SetStringField(TEXT("dir"), GameTestShot::ToProjectRelative(Dir));

	UWorld* World = GameTestWorld::FindGameWorld();

	// 1. 截图：失败那一刻屏上是什么
	{
		const FString ShotPath = FPaths::Combine(Dir, TEXT("screen.png"));
		FString Message;
		TSharedPtr<FJsonObject> Ignored;
		if (GameTestShot::CaptureGameWindow(World, ShotPath, nullptr, Message, Ignored))
		{
			Evidence->SetStringField(TEXT("screenshot"), GameTestShot::ToProjectRelative(ShotPath));
		}
		else
		{
			Evidence->SetStringField(TEXT("screenshotError"), Message);
		}
	}

	// 2. 控件快照：屏上有哪些控件；步骤有目标时再解析一次，找不到就留下候选，找到了说明「在，但没点成」
	if (World != nullptr)
	{
		TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
		GameTestSnapshot::CaptureScreens(World, Screens, /*bInLight=*/true);
		const FString WidgetsPath = FPaths::Combine(Dir, TEXT("widgets.json"));
		const TSharedPtr<FJsonObject> Tree = GameTestSnapshot::ScreensToJson(Screens, /*bInIncludeGeometry=*/true, /*bInIncludeProps=*/false, EvidenceMaxNodes);
		if (FFileHelper::SaveStringToFile(GameTestJson::ToPrettyString(Tree), *WidgetsPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Evidence->SetStringField(TEXT("widgets"), GameTestShot::ToProjectRelative(WidgetsPath));
		}
		Evidence->SetNumberField(TEXT("screens"), Screens.Num());
	}

	const FString& Target = InOutRecord.Step.Target;
	if (!Target.IsEmpty())
	{
		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		if (GameTestHandlers::TryResolveTarget(Target, InHandles, Node, Error) && Node.IsValid())
		{
			const TSharedPtr<FJsonObject> Found = MakeShared<FJsonObject>();
			Found->SetStringField(TEXT("path"), Node->Path);
			Found->SetStringField(TEXT("class"), Node->ClassName);
			if (!Node->Text.IsEmpty())
			{
				Found->SetStringField(TEXT("text"), Node->Text);
			}
			Found->SetStringField(TEXT("visibility"), Node->Visibility);
			Found->SetBoolField(TEXT("enabled"), Node->bEnabled);
			Evidence->SetObjectField(TEXT("target"), Found);
		}
		else
		{
			Evidence->SetStringField(TEXT("selectorError"), Error.Left(EvidenceMaxSelectorChars));
		}
	}

	// 3. 日志：先写「之前」，「之后」等攒够了再补
	{
		FPendingLog Pending;
		Pending.Path = FPaths::Combine(Dir, TEXT("log.txt"));
		Pending.FailSeq = LogTap.GetSeq();
		Pending.Before = LogTap.Range(Pending.FailSeq > LogLines ? Pending.FailSeq - LogLines : 0, Pending.FailSeq);
		Pending.Deadline = FPlatformTime::Seconds() + EvidenceLogWaitSeconds;
		WriteLog(Pending, /*bInFinal=*/false);
		PendingLogs.Add(MoveTemp(Pending));
		Evidence->SetStringField(TEXT("log"), GameTestShot::ToProjectRelative(PendingLogs.Last().Path));
	}

	if (!InOutRecord.Payload.IsValid())
	{
		InOutRecord.Payload = MakeShared<FJsonObject>();
	}
	InOutRecord.Payload->SetObjectField(TEXT("evidence"), Evidence);
}

void FGameTestEvidence::Tick(double InNowSeconds)
{
	if (PendingLogs.Num() == 0)
	{
		return;
	}

	const uint64 Seq = LogTap.GetSeq();
	for (int32 Index = PendingLogs.Num() - 1; Index >= 0; --Index)
	{
		const FPendingLog& Pending = PendingLogs[Index];
		if (Seq >= Pending.FailSeq + LogLines || InNowSeconds >= Pending.Deadline)
		{
			WriteLog(Pending, /*bInFinal=*/true);
			PendingLogs.RemoveAt(Index);
		}
	}
}

void FGameTestEvidence::FlushAll()
{
	for (const FPendingLog& Pending : PendingLogs)
	{
		WriteLog(Pending, /*bInFinal=*/true);
	}
	PendingLogs.Reset();
}

void FGameTestEvidence::WriteLog(const FPendingLog& InPending, bool bInFinal) const
{
	TArray<FString> Lines = InPending.Before;
	Lines.Add(TEXT("—————— 这一步在这里失败 ——————"));
	if (bInFinal)
	{
		Lines.Append(LogTap.Range(InPending.FailSeq, InPending.FailSeq + LogLines));
	}
	else
	{
		Lines.Add(TEXT("（之后的日志还在收集）"));
	}
	FFileHelper::SaveStringToFile(FString::Join(Lines, TEXT("\n")) + TEXT("\n"), *InPending.Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
