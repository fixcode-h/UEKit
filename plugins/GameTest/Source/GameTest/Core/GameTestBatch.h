// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
struct FGameTestReport;

/** 批量运行里的一个场景。 */
struct FGameTestBatchItem
{
	/** 相对场景根的路径。 */
	FString Scenario;
	FString Name;
	FString RunId;
	/** pending / running / passed / failed / error（没能开始：文件坏了、解析没过）/ skipped（被取消）。 */
	FString Status = TEXT("pending");
	FString Message;
	int32 FailedStep = 0;
	double DurationMs = 0.0;
};

/**
 * 批量运行（`gt.run tag=…` 或 `scenarios=[…]`）：一个接一个跑，前一个结束下一帧起下一个，一个失败不影响后面的。
 *
 * 只管「排队和记账」，不起 run、不读文件——那是子系统的事（它持有执行器，同一时刻只能有一个 run）。
 * 纯逻辑，可脱离引擎单测。只在游戏线程上用。
 */
class GAMETEST_API FGameTestBatch
{
public:
	void Start(const FString& InId, const FString& InLabel, const TArray<FString>& InScenarios);

	bool IsActive() const
	{
		return bActive;
	}

	/** 有没有一份（进行中或刚结束的）批量可以报告。 */
	bool HasData() const
	{
		return Items.Num() > 0;
	}

	/** 取下一个待跑的，标成 running 并返回它的序号；没有了返回 INDEX_NONE。 */
	int32 TakeNext();

	const FGameTestBatchItem& GetItem(int32 InIndex) const
	{
		return Items[InIndex];
	}

	/** 起 run 之后记下 runId 和场景名（run 可能已经在起的那一下跑完了，已经记过的不覆盖）。 */
	void MarkStarted(int32 InIndex, const FString& InRunId, const FString& InName);

	/** 没能开始（文件坏了、解析没过、执行器忙）。 */
	void MarkError(int32 InIndex, const FString& InMessage);

	/** 一次 run 结束：记到正在跑的那一项上。不是批量里的 run 忽略。 */
	void OnRunFinished(const FGameTestReport& InReport);

	/** 取消：正在跑的由执行器标失败，没轮到的标 skipped，批量结束。 */
	void Cancel();

	/** 都跑完了就结束批量；返回这一下是否刚结束。 */
	bool FinishIfDone();

	/** 不是批量的 run 开始前清掉上一份批量的结果，作业快照里不再带它。 */
	void Clear();

	/** 每次有变化 +1：子系统比对它决定要不要刷新作业快照、推送事件。 */
	int64 GetSerial() const
	{
		return Serial;
	}

	int32 Count(const TCHAR* InStatus) const;

	TSharedPtr<FJsonObject> ToJson() const;

private:
	FString Id;
	FString Label;
	TArray<FGameTestBatchItem> Items;
	int32 Current = INDEX_NONE;
	bool bActive = false;
	int64 Serial = 0;
};
