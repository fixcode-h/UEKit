// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/GameTestRunner.h"
#include "Core/GameTestRunStore.h"
#include "Subsystems/EngineSubsystem.h"
#include "GameTestSubsystem.generated.h"

class FJsonObject;
class UWidget;

/** 高亮覆盖层的持有者：Slate 类型同样只留在 .cpp 里。 */
struct FGameTestHighlightHolder;

struct FGameTestHighlightHolderDeleter
{
	void operator()(FGameTestHighlightHolder* InHolder) const;
};

/**
 * 常驻宿主（编辑器与游戏进程都有）。
 *
 * 它拥有三样东西：执行器、名称句柄表、需要保活的对象。对外的通道是 UELink（`gt.*` 操作，见
 * `Integration/GameTestRpc`），子系统本身不认识它。
 * 放在 UEngineSubsystem 而不是 GameInstanceSubsystem，是因为后者只在某个游戏世界里存在——
 * 而"先问编辑器能不能 PIE""PIE 还没起来就要收到场景请求"这些事发生在世界之外。
 *
 * **线程契约：入口都必须在游戏线程上调用。** UELink 的操作本来就在游戏线程执行，所以操作里
 * **绝不能**再"等游戏线程"——那是对自己等待，会把自己锁死到超时。正确做法是就地执行，跨帧步骤交给 ticker。
 */
UCLASS()
class GAMETEST_API UGameTestSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	UGameTestSubsystem();
	virtual ~UGameTestSubsystem();

	/**
	 * 取子系统（引擎未就绪时返回 nullptr）。
	 * 做成类的静态成员而不是自由函数：导出宏一次覆盖，别的模块（编辑器窗口）要用也不必再声明一遍。
	 */
	static UGameTestSubsystem* Get();

	//~ Begin UEngineSubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& InCollection) override;
	virtual void Deinitialize() override;
	//~ End UEngineSubsystem interface

	/** 游戏线程 tick：驱动执行器，并维护 `job` 的快照。 */
	bool Tick(float InDeltaSeconds);

	/**
	 * 游戏线程：执行一步。
	 * 同步步骤（大多数）当场给结果；跨帧步骤（wait / pie.start）转入异步，
	 * 此时返回体里带 runId，进度经 `GameTestHooks::OnJobChanged` 推出去（UELink 的 gt.job 事件）。
	 */
	void RunStepInline(const TSharedPtr<FJsonObject>& InStepJson, TSharedPtr<FJsonObject>& OutResult);

	/**
	 * 游戏线程：起一次整场景 run，立即返回 runId；失败时 OutResult 是原因。
	 * `InStepDelayMs` ≥ 0 时覆盖设置里的步间隔（场景的 `stepDelayMs`，录制的场景写 0）。
	 */
	bool BeginScenarioRun(
		const TArray<TSharedPtr<FJsonObject>>& InSteps,
		const FString& InName,
		int32 InRevision,
		FString& OutRunId,
		TSharedPtr<FJsonObject>& OutResult,
		int32 InStepDelayMs = -1);

	void CancelCurrentRun();

	TSharedPtr<FJsonObject> BuildHealthJson() const;
	TSharedPtr<FJsonObject> BuildJobJson() const;
	TSharedPtr<FJsonObject> BuildActionsJson() const;

	/** 场景根目录下全部场景的清单（窗口与 `gt.scenarios` 共用 `GameTestScenario::ScanDirectory`）。 */
	TSharedPtr<FJsonObject> BuildScenariosJson() const;

	/** 本次会话的 run 摘要清单，最近一次在最前。 */
	TSharedPtr<FJsonObject> BuildRunsJson() const;

	/** 指定 runId 的完整报告；查不到时返回失败并说明。 */
	TSharedPtr<FJsonObject> BuildRunJson(const FString& InRunId) const;

private:
	/**
	 * 解析并起 run，然后立刻 tick 一次：同步步骤在本次调用里就有结果。
	 * @param bOutAlreadyFinished 起完之后是否已经跑完（跑完就没有 runId 可用）。
	 */
	bool StartRunOnGameThread(
		const TArray<TSharedPtr<FJsonObject>>& InSteps,
		const FString& InName,
		int32 InRevision,
		FString& OutRunId,
		bool& bOutAlreadyFinished,
		TSharedPtr<FJsonObject>& OutResult,
		int32 InStepDelayMs = -1);

	/**
	 * 把一次 run 的报告写到 `<InOutputDir>/report.json`（成功/失败/取消三条收尾路径共用）。
	 * 写盘失败只记警告——报告是证物，不是被测对象，存不下不该改判 run 的结论。
	 */
	void WriteReportFile(const FGameTestReport& InReport, const FString& InOutputDir) const;

	/** 一步/一次 run 的结果 → 返回体。 */
	TSharedPtr<FJsonObject> BuildOutcomeJson(bool bInAlreadyFinished) const;

	/**
	 * run 期间关掉"编辑器不在前台就节流"，结束恢复。
	 * 不这么做的话：窗口没焦点 → 引擎几乎不 Tick、渲染也停 → PIE 世界与截图一起废掉。
	 */
	void ApplyBackgroundThrottleOverride();
	void RestoreBackgroundThrottle();

	/**
	 * 把执行器刚碰过的控件框在游戏窗口最上层（每次 tick 调，只在确有变化时才动覆盖层）。
	 *
	 * 没有视口（命令行 / 无头）时静默跳过——这条是给人眼看的，不该让任何一步因此失败。
	 */
	void UpdateHighlight(double InNowSeconds);
	void ClearHighlight();

	TUniquePtr<FGameTestRunner> Runner;
	TUniquePtr<FGameTestHighlightHolder, FGameTestHighlightHolderDeleter> Highlight;

	/** 本次会话的 run 历史（`gt.runs` 的回查范围）；每次收尾的完整报告也同时落盘。 */
	GameTestRunStore RunHistory;

	FTSTicker::FDelegateHandle TickerHandle;

	/** 名称句柄：跨 run 有效，世界重载时清掉。 */
	TMap<FString, TWeakObjectPtr<UWidget>> Handles;

	/** 需要活过 PIE 会话启动的临时对象。 */
	TArray<TStrongObjectPtr<UObject>> RootedObjects;

	FString CurrentRunId;

	/** job 快照：游戏线程写；加锁是给别的线程（日志、调试）读的。 */
	mutable FCriticalSection JobLock;
	TSharedPtr<FJsonObject> JobSnapshot;
	int32 LastSnapshotStepCount = -1;
	bool LastSnapshotBusy = false;
	FString LastSnapshotRunId;

	FThreadSafeCounter RunCounter;

	bool bThrottleOverridden = false;
	bool bPreviousThrottleValue = false;

	/** 活性证据：ticker 被调用的次数（health 里报出去，用来判断游戏线程是不是真的在走）。 */
	int64 TickCount = 0;
	double LastHeartbeatLog = 0.0;
};
