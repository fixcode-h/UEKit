// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Flow/GameTestNode.h"
#include "Observe/Checks/GameTestCond.h"

class FGameTestFlowContext;
class FGameTestScriptParser;
class FJsonObject;
class FJsonValue;

/** 守护什么时候看（协议第 5 节）。 */
enum class EGameTestGuardCheck : uint8
{
	BeforeStep,
	WhileWaiting,
	Both,
};

/** 守护处理完怎么接着走。 */
enum class EGameTestGuardAfter : uint8
{
	/** 回到被打断的那一步重新开始（默认）。 */
	Retry,
	/** 那一步算跳过。 */
	Skip,
	Fail,
};

/** 一条守护的定义（加载时解析好）。 */
struct GAMETEST_API FGameTestGuardDef
{
	FString Id;
	FString Name;
	/** 在哪定义的：`global`（_settings.json）、`script`（场景顶层）、`block`（块的 guards）。 */
	FString Scope;
	/** 定义位置的路径（`guards.0`、`steps.3.guards.1`），没 id 时按它认。 */
	FString Path;
	TSharedPtr<FJsonObject> When;
	FGameTestNodeList Do;
	EGameTestGuardCheck Check = EGameTestGuardCheck::Both;
	EGameTestGuardAfter After = EGameTestGuardAfter::Retry;
	int32 Max = 20;
	int32 CooldownMs = 1000;
	bool bUnlessStepTargets = true;
	/** `onFail: ignore`：do 失败只记警告。 */
	bool bIgnoreFail = false;
	bool bDisabled = false;
	/** `when` 里出现的界面名（unlessStepTargets 比对用）。 */
	TArray<FString> Screens;

	const FString& Key() const { return Id.IsEmpty() ? Path : Id; }
	FString Label() const { return Name.IsEmpty() ? Key() : Name; }
};

namespace GameTestGuards
{
	/** 守护对象能写的字段。 */
	GAMETEST_API const TArray<FString>& Fields();

	/** 解析 `guards` 数组（场景顶层、块、运行配置）。`InPath` 是数组的路径（`guards`、`steps.3.guards`）。 */
	GAMETEST_API void ParseList(const TSharedPtr<FJsonValue>& InJson, const FString& InScope, const FString& InPath, FGameTestScriptParser& InParser,
		TArray<TSharedRef<FGameTestGuardDef>>& OutGuards);

	/**
	 * 阻挡界面迁移（协议第 5.1 节）：`_settings.json` 里的 `blockers` 改写成 `guards`（纯数据变换，不读写文件）。
	 * 返回 true 表示改了；`OutNotes` 记每条怎么改的（写日志）。
	 */
	GAMETEST_API bool MigrateBlockers(FJsonObject& InOutSettings, TArray<FString>& OutNotes);
}

/**
 * 守护的运行时：每条守护的触发次数、冷却、稳住，以及挑「现在该处理哪一条」。
 * 执行器持有一个；次数进 `gt.job guards` 和报告。只在游戏线程上用。
 */
class GAMETEST_API FGameTestGuardMonitor
{
public:
	void Reset();

	/**
	 * 看一遍生效的守护（`InActive` 已按 块内层 → 脚本 → 全局 排好），返回第一条该处理的，没有返回 nullptr。
	 * `bWaiting`：在等待中（只看 whileWaiting / both），否则是每步开始前（beforeStep / both）。
	 * `InStepTargets`：正在执行的那一步的 target / toTarget（unlessStepTargets 用）。
	 * `OutPending`：有守护的 when 已经成立、还在稳住——每步开始前遇到它要等一等再开始这一步，不然稳住永远等不到。
	 */
	const FGameTestGuardDef* Poll(const TArray<TSharedRef<FGameTestGuardDef>>& InActive, bool bWaiting, const TArray<FString>& InStepTargets,
		FGameTestFlowContext& InContext, double InNowSeconds, bool& OutPending);

	/** 开始处理：次数 +1。超过 max 返回 false（`OutError` 写 guard_max 的原因）。 */
	bool BeginHandle(const FGameTestGuardDef& InGuard, double InNowSeconds, FString& OutError);

	/**
	 * 处理完：再判断一次 `when`。还成立记 stillThere，连续 3 次还成立返回 false（guard_stuck）。
	 * `OutStillThere` 给报告。
	 */
	bool EndHandle(const FGameTestGuardDef& InGuard, FGameTestFlowContext& InContext, double InNowSeconds, bool& OutStillThere, FString& OutError);

	int32 GetCount(const FString& InKey) const;

	/** `{id: 次数}`（gt.job、报告）。 */
	TSharedPtr<FJsonObject> CountsToJson() const;

	/** 下一次等待中检查的时刻（每 0.2 秒）。 */
	bool IsWaitCheckDue(double InNowSeconds) const { return InNowSeconds >= NextWaitCheckAt; }
	void MarkWaitChecked(double InNowSeconds) { NextWaitCheckAt = InNowSeconds + 0.2; }

private:
	struct FState
	{
		int32 Count = 0;
		double LastAt = -1.0;
		int32 StillThere = 0;
		FGameTestCondWatch Watch;
		bool bWatching = false;
	};

	TMap<FString, FState> States;
	double NextWaitCheckAt = 0.0;
};
