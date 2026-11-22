// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FGameTestLogTap;
class FJsonObject;
class UWidget;
struct FGameTestStepRecord;

/**
 * 一步失败时自动留下的证据：截图、控件快照（选择器解析的结果或候选）、这一步用到的对象的属性（2.3.1）、失败前后 50 行日志。
 *
 * 写进 `<run 产物目录>/failures/step-<第几步>/`，报告里只记路径（`payload.evidence`），不把内容塞进返回：
 * 截图和控件树动辄几十 KB，Agent 要看时按路径读（面板 ADR 0067 的返回长度规则）。
 *
 * 日志分两段写：失败那一刻先写「之前 50 行」，之后每帧看一眼，再攒够 50 行或过了 3 秒就把「之后」补上。
 * 不在失败时干等——游戏线程不能卡。由子系统持有，只在游戏线程上用。
 */
class GAMETEST_API FGameTestEvidence
{
public:
	/** 一次 run 最多留几份：Continue 模式下一路失败时不至于截几十张图。 */
	static constexpr int32 MaxPerRun = 5;

	/** 前后各留多少行日志（刷屏的类别折叠之后）。 */
	static constexpr int32 LogLines = 50;

	/** 前后各从多少行原始日志里挑：有类别刷屏时，折叠后还要剩得下 LogLines 行有用的。 */
	static constexpr int32 RawLogLines = 600;

	explicit FGameTestEvidence(FGameTestLogTap& InLogTap);

	/** 新的一次 run 开始：重置本次的份数。 */
	void BeginRun();

	/**
	 * 一步失败：留证据，往记录的 Payload 里写 `evidence`（路径都相对项目根）。
	 * 截图或快照失败只记原因，不影响这一步的结论——证据是给人看的，不是被测对象。
	 */
	void Capture(FGameTestStepRecord& InOutRecord, const FString& InRunDir, TMap<FString, TWeakObjectPtr<UWidget>>* InHandles);

	/** 每帧调：日志的「之后」攒够了或到点了就补进文件。 */
	void Tick(double InNowSeconds);

	/** 子系统关闭时：还在等的日志立刻写完。 */
	void FlushAll();

private:
	struct FPendingLog
	{
		FString Path;
		TArray<FString> Before;
		uint64 FailSeq = 0;
		double Deadline = 0.0;
		/** 「要紧的行」从这一刻起取：本次 run 开始的时间。 */
		double ImportantFrom = 0.0;
	};

	void WriteLog(const FPendingLog& InPending, bool bInFinal) const;

	/** 这一步引用解析到的对象（payload.ref.path）此刻的属性写进 object.json，证据里记 `object`。 */
	void CaptureObject(const FGameTestStepRecord& InRecord, const FString& InDir, const TSharedPtr<FJsonObject>& InOutEvidence) const;

	FGameTestLogTap& LogTap;
	TArray<FPendingLog> PendingLogs;
	int32 CapturedThisRun = 0;
	double RunStartedAt = 0.0;
};
