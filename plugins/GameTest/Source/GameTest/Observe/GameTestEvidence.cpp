// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestEvidence.h"

#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestLogTap.h"
#include "Core/GameTestTypes.h"
#include "Core/GameTestWorld.h"
#include "Integration/Ref/GameTestRpcRef.h"
#include "Observe/GameTestSnapshot.h"
#include "Observe/GameTestWidgetNode.h"
#include "Shot/GameTestShot.h"

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Components/Widget.h"

namespace
{
	/** 「之后」的日志最多等多久：Stop 模式下失败后马上收尾，等不来 50 行也要把文件写完整。 */
	constexpr double EvidenceLogWaitSeconds = 3.0;

	/** 控件快照的节点上限：够看清失败时屏上有什么，又不至于整棵树几 MB。 */
	constexpr int32 EvidenceMaxNodes = 3000;

	/** 用到的对象的属性最多读几项：gt.props 的上限，够看清失败那一刻它的状态。 */
	constexpr int32 EvidenceMaxProps = 400;

	/** 选择器候选的说明截断：候选清单可能很长，证据里放够判断的那一段。 */
	constexpr int32 EvidenceMaxSelectorChars = 2000;

	/** 一段日志里某个类别超过这么多行就算刷屏：只留它的警告和错误，其余折成一句「省略了几行」。 */
	constexpr int32 EvidenceSpamLines = 20;

	/** 日志行「类别: 内容」或「类别: 级别: 内容」→ 类别。 */
	FString EvidenceCategoryOf(const FString& InLine)
	{
		int32 Colon = INDEX_NONE;
		return InLine.FindChar(TEXT(':'), Colon) ? InLine.Left(Colon) : FString();
	}

	bool EvidenceIsWarningOrError(const FString& InLine, const FString& InCategory)
	{
		const FString Rest = InLine.RightChop(InCategory.Len() + 1).TrimStart();
		return Rest.StartsWith(TEXT("Warning:")) || Rest.StartsWith(TEXT("Error:")) || Rest.StartsWith(TEXT("Fatal:"));
	}

	/**
	 * 折叠刷屏：一段里行数超过阈值的类别（LogGameTest 除外）只留警告和错误，省掉的按类别记数。
	 * 项目里每帧打印的 Lua 日志会把失败前后几百行全占满，真正有用的几行被淹掉。
	 */
	TArray<FString> EvidenceFoldSpam(const TArray<FString>& InLines, TMap<FString, int32>& InOutDropped)
	{
		TMap<FString, int32> Counts;
		for (const FString& Line : InLines)
		{
			++Counts.FindOrAdd(EvidenceCategoryOf(Line));
		}
		TArray<FString> Kept;
		for (const FString& Line : InLines)
		{
			const FString Category = EvidenceCategoryOf(Line);
			const bool bSpam = Category != TEXT("LogGameTest") && Counts.FindRef(Category) > EvidenceSpamLines
				&& !EvidenceIsWarningOrError(Line, Category);
			if (bSpam)
			{
				++InOutDropped.FindOrAdd(Category);
				continue;
			}
			Kept.Add(Line);
		}
		return Kept;
	}
}

FGameTestEvidence::FGameTestEvidence(FGameTestLogTap& InLogTap)
	: LogTap(InLogTap)
{
}

void FGameTestEvidence::BeginRun()
{
	CapturedThisRun = 0;
	RunStartedAt = FPlatformTime::Seconds();
}

void FGameTestEvidence::Capture(FGameTestStepRecord& InOutRecord, const FString& InRunDir, TMap<FString, TWeakObjectPtr<UWidget>>* InHandles)
{
	if (InRunDir.IsEmpty() || CapturedThisRun >= MaxPerRun)
	{
		return;
	}
	++CapturedThisRun;

	// Printf 的格式串必须是字面量（新引擎编译期校验），所以两种前缀各写一遍。
	const FString Dir = FPaths::Combine(InRunDir, TEXT("failures"), InOutRecord.Step.bSetup
		? FString::Printf(TEXT("setup-%02d"), InOutRecord.Step.Index + 1)
		: FString::Printf(TEXT("step-%02d"), InOutRecord.Step.Index + 1));
	IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);

	const TSharedPtr<FJsonObject> Evidence = MakeShared<FJsonObject>();
	Evidence->SetStringField(TEXT("dir"), GameTestShot::ToProjectRelative(Dir));

	// 正在关闭的世界（PIE 刚停）不去碰：截图、控件快照都按「没有世界」写进证据，不崩。
	UWorld* World = GameTestWorld::FindGameWorld();
	if (World != nullptr && (!IsValid(World) || World->bIsTearingDown))
	{
		World = nullptr;
	}

	// 1. 截图：失败那一刻屏上是什么
	{
		const FString ShotPath = FPaths::Combine(Dir, TEXT("screen.png"));
		FString Message;
		// 截图会往这里写尺寸等字段：必须给一个真对象（以前传空指针，停止 run 时一失败就在截图里崩了）。
		TSharedPtr<FJsonObject> Ignored = MakeShared<FJsonObject>();
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

	// 3. 这一步用到的对象（obj.call、obj.set、检查对象属性……引用解析时在 payload.ref 记了路径）：
	// 失败那一刻把它的属性读一次存成 object.json（和 gt.props 返回同一种形状，面板直接按细节面板画），
	// 不用再开游戏重跑去看「当时那个对象是什么状态」。对象这时已经没了只记原因
	CaptureObject(InOutRecord, Dir, Evidence);

	// 4. 日志：先写「之前」，「之后」等攒够了再补
	{
		FPendingLog Pending;
		Pending.Path = FPaths::Combine(Dir, TEXT("log.txt"));
		Pending.FailSeq = LogTap.GetSeq();
		Pending.Before = LogTap.Range(Pending.FailSeq > RawLogLines ? Pending.FailSeq - RawLogLines : 0, Pending.FailSeq);
		Pending.ImportantFrom = RunStartedAt;
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

void FGameTestEvidence::CaptureObject(const FGameTestStepRecord& InRecord, const FString& InDir, const TSharedPtr<FJsonObject>& InOutEvidence) const
{
	if (!InRecord.Payload.IsValid() || !InOutEvidence.IsValid())
	{
		return;
	}
	const TSharedPtr<FJsonObject>* Ref = nullptr;
	if (!InRecord.Payload->TryGetObjectField(TEXT("ref"), Ref) || Ref == nullptr || !(*Ref).IsValid())
	{
		return;
	}
	FString ObjectPath;
	if (!(*Ref)->TryGetStringField(TEXT("path"), ObjectPath) || ObjectPath.IsEmpty())
	{
		return;
	}
	const TSharedPtr<FJsonObject> Found = MakeShared<FJsonObject>();
	Found->SetStringField(TEXT("path"), ObjectPath);
	FString Picked;
	if ((*Ref)->TryGetStringField(TEXT("picked"), Picked) && !Picked.IsEmpty())
	{
		Found->SetStringField(TEXT("name"), Picked);
	}
	FString ClassName;
	if ((*Ref)->TryGetStringField(TEXT("class"), ClassName) && !ClassName.IsEmpty())
	{
		Found->SetStringField(TEXT("class"), ClassName);
	}

	// 和面板点开实例时同一条路（gt.props 按 path 读），同类比较不做：证据只要这一个对象此刻的值
	const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("path"), ObjectPath);
	Args->SetBoolField(TEXT("siblings"), false);
	Args->SetNumberField(TEXT("limit"), EvidenceMaxProps);
	TSharedPtr<FJsonObject> Props;
	FString Error;
	if (GameTestRpcRef::Props(Args, Props, Error) && Props.IsValid())
	{
		const FString PropsPath = FPaths::Combine(InDir, TEXT("object.json"));
		if (FFileHelper::SaveStringToFile(GameTestJson::ToPrettyString(Props), *PropsPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Found->SetStringField(TEXT("props"), GameTestShot::ToProjectRelative(PropsPath));
		}
		else
		{
			Found->SetStringField(TEXT("error"), TEXT("属性读到了，但写不进证据目录（磁盘满或没有权限）"));
		}
	}
	else
	{
		Found->SetStringField(TEXT("error"), Error.IsEmpty() ? FString(TEXT("读不到这个对象的属性")) : Error.Left(EvidenceMaxSelectorChars));
	}
	InOutEvidence->SetObjectField(TEXT("object"), Found);
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
		if (Seq >= Pending.FailSeq + RawLogLines || InNowSeconds >= Pending.Deadline)
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
	TArray<FString> Lines;

	// 1. 要紧的行：本次 run 里 GameTest 每一步的结果、每次点击送到了哪，以及任何类别的警告和错误。
	Lines.Add(TEXT("== GameTest 每一步和警告 / 错误（本次 run 开始以来） =="));
	Lines.Append(LogTap.ImportantSince(InPending.ImportantFrom));
	Lines.Add(FString());

	// 2. 失败前后的原始日志：刷屏的类别折叠掉，只留前后各 LogLines 行。
	TMap<FString, int32> Dropped;
	TArray<FString> Before = EvidenceFoldSpam(InPending.Before, Dropped);
	if (Before.Num() > LogLines)
	{
		Before.RemoveAt(0, Before.Num() - LogLines);
	}
	TArray<FString> After;
	if (bInFinal)
	{
		After = EvidenceFoldSpam(LogTap.Range(InPending.FailSeq, InPending.FailSeq + RawLogLines), Dropped);
		if (After.Num() > LogLines)
		{
			After.SetNum(LogLines);
		}
	}

	Lines.Add(TEXT("== 失败前后的日志 =="));
	if (Dropped.Num() > 0)
	{
		TArray<FString> Parts;
		for (const TPair<FString, int32>& Pair : Dropped)
		{
			Parts.Add(FString::Printf(TEXT("%s %d 行"), *Pair.Key, Pair.Value));
		}
		Lines.Add(FString::Printf(TEXT("（刷屏的类别只留警告和错误，省略了：%s）"), *FString::Join(Parts, TEXT("、"))));
	}
	Lines.Append(Before);
	Lines.Add(TEXT("—————— 这一步在这里失败 ——————"));
	if (bInFinal)
	{
		Lines.Append(After);
	}
	else
	{
		Lines.Add(TEXT("（之后的日志还在收集）"));
	}
	FFileHelper::SaveStringToFile(FString::Join(Lines, TEXT("\n")) + TEXT("\n"), *InPending.Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
