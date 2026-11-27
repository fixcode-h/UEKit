// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Flow/GameTestFlow.h"
#include "Core/GameTestTypes.h"

class FJsonObject;
class FJsonValue;
class FGameTestFlowContext;
class FGameTestLogTap;
class FGameTestSkipGate;
class UWidget;
class UWorld;
struct FGameTestWidgetNode;

/**
 * 一步执行时能看到、能改的东西。
 *
 * 它同时是执行器与动作实现之间的唯一契约：动作不直接碰子系统，
 * 需要的东西（世界、句柄表、保活槽、产物目录）都从这里拿。
 */
struct FGameTestStepContext
{
	const FGameTestStep* Step = nullptr;
	UWorld* World = nullptr;

	/**
	 * 脚本上下文（1.9.0）：变量、对象、只读闸门、本次 run 的日志序号。动作里求值、调接口、存 saveAs 都用它。
	 * 执行器持有，一次 run 里地址不变；gt.step 单步也会给一个。
	 */
	FGameTestFlowContext* Flow = nullptr;

	/** 名称句柄表，由子系统持有——跨 HTTP 调用（=跨 run）也要能用 `@句柄`。 */
	TMap<FString, TWeakObjectPtr<UWidget>>* Handles = nullptr;

	/** 需要活过本次会话的对象（例如 PIE 的临时 play settings），由子系统强引用住。 */
	TArray<TStrongObjectPtr<UObject>>* RootedObjects = nullptr;

	/** 本次 run 的产物目录（绝对路径）。 */
	FString OutputDir;

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	FString Message;

	/**
	 * 这一步解析到的目标控件（选择器命中的那个）与它在屏幕上的矩形。高亮层读它把控件框出来。
	 * 失败也填——"没点动的是哪个控件"同样要看得见。
	 *
	 * 矩形用的是**快照那套坐标**（与截图标注同一份数，已验证）：高亮层不该自己去读 Slate 几何，
	 * 那条路上有多层 DPI/空间换算，实测很容易画歪（框小一圈、或飘到屏幕中间）。
	 */
	TWeakObjectPtr<UWidget> TouchedWidget;
	bool bTouchedHasRect = false;
	FVector2D TouchedMin = FVector2D::ZeroVector;
	FVector2D TouchedMax = FVector2D::ZeroVector;

	/**
	 * 跨帧动作（wait / pie.start / pie.stop / editor.ensure）填这个：
	 * 执行器每帧求值一次，返回 true 即完成；返回 Pending 却不填谓词会被判失败。
	 */
	TFunction<bool(double /*NowSeconds*/)> PendingPredicate;

	/**
	 * 跨帧步骤的**进度描述**，由谓词每帧更新（例：`actorNear` 写"当前距离 812.3"）。
	 * 谓词返回 false 且最终超时时，执行器把最新这条拼进失败信息——"还差多远"是超时唯一有用的线索，
	 * 否则调用方只知道"没等到"。为 nullptr 时不拼。
	 */
	TSharedPtr<FString> PendingProgress;

	/**
	 * 跨帧步骤**中途就确定失败**时（例：点下去接住的不是目标控件）由谓词写进这里再返回 true：
	 * 执行器看到非空就判失败、用这句话当消息，不用干等到超时。
	 */
	TSharedPtr<FString> PendingFailure;

	/**
	 * 跨帧步骤结束时（完成、失败、超时、取消都算）执行器调一次：用来收拾半路留下的状态，
	 * 例如真实点击按下后超时，鼠标键不能一直按着，后面的步骤会在「左键按住」下跑。
	 */
	TFunction<void()> PendingCleanup;

	/**
	 * 动作自己的默认超时（毫秒）：步骤没写 `timeoutMs` 时用它代替全局的单步默认超时。
	 * UI 动作的自动等待用 `UiWaitDefault()`。小于 0 表示不设上限：一直等，直到完成或有人 stop。
	 */
	int32 DefaultTimeoutMs = 0;

	/** 本次 run 的 UI 等待上限（毫秒，0 = 不限），见 FGameTestRunRequest::UiWaitTimeoutMs。 */
	int32 UiWaitTimeoutMs = 0;

	/** 本次 run 里 click 没写 via 时的点击方式（code / pointer），见 FGameTestRunRequest::ClickVia。 */
	FString ClickVia;

	/** 本次 run 的 move 默认设置（运行设置和场景的 move 段合起来），move 步骤的字段盖在上面；没有时为空。 */
	TSharedPtr<FJsonObject> MoveDefaults;

	/** UI 动作该填进 DefaultTimeoutMs 的值：不限时给 -1。 */
	int32 UiWaitDefault() const
	{
		return UiWaitTimeoutMs > 0 ? UiWaitTimeoutMs : -1;
	}
};

using FGameTestHandler = EGameTestStepStatus (*)(FGameTestStepContext&);
using FGameTestOnFinished = TFunction<void(const FGameTestReport&)>;

/**
 * run 期间由**子系统**持有、经 context 交给动作的几本账。
 *
 * 收成一个结构体是因为 `FGameTestRunner::StartRun` 的参数已到上限（5 个）：再加一本账就超预算，
 * 而这几样本就是同一件事（"动作能读到/改到的跨步状态"）。
 * 生命周期由子系统保证——执行器只借用，不拥有。
 */
struct GAMETEST_API FGameTestRuntimeState
{
	/** 名称句柄表（跨 HTTP 调用也要能用 `@句柄`）。 */
	TMap<FString, TWeakObjectPtr<UWidget>>* Handles = nullptr;

	/** 需要活过本次会话的对象（例如 PIE 的临时 play settings）。 */
	TArray<TStrongObjectPtr<UObject>>* RootedObjects = nullptr;

	/**
	 * 一步失败时（取消不算）调一次，此时记录还没进报告：子系统在这里留失败证据（截图、控件、日志），
	 * 把路径写进记录的 Payload。执行器只给时机，不知道证据是什么——它是算法层，不碰 Slate 和日志。
	 * 为空时不留。
	 */
	TFunction<void(FGameTestStepRecord& /*Record*/, const FString& /*OutputDir*/)> OnStepFailed;

	/** 日志环（1.9.0）：脚本的 Log.* 接口、expectNoErrors 用。 */
	FGameTestLogTap* LogTap = nullptr;

	/** `gt.flow` 推送（节点进出、循环换轮、守护、暂停）；为空时不推。 */
	TFunction<void(const TSharedRef<FJsonObject>&)> OnFlowEvent;
};

/**
 * 一次 run 的状态机（只在游戏线程被 Tick）。
 *
 * 两条硬规则：
 * 1. **一步一帧**：同步步骤执行完就结束本帧，下一步留到下一帧——给 UMG 一个刷新的机会，
 *    也让"点完按钮马上看界面"这类场景有确定的时序。
 * 2. **等待是谓词，不是 sleep**：跨帧动作返回 Pending 并给一个每帧求值的谓词，
 *    超过 `timeoutMs` 才判失败。游戏线程永远不被阻塞。
 */
class GAMETEST_API FGameTestRunner
{
public:
	/**
	 * 开始一次 run。同一时刻只允许一个：已经有 run 在跑时返回 false（调用方负责给出提示）。
	 * `InRuntimeState` 里的三本账由调用方（子系统）持有并保证生命周期。
	 */
	bool StartRun(const FGameTestRunRequest& InRequest, const FGameTestRuntimeState& InRuntimeState, FGameTestOnFinished InOnFinished, FString& OutError);

	bool IsBusy() const
	{
		return bActive;
	}

	void Tick(double InNowSeconds);

	/** 请求取消：当前步标失败，余下步骤标跳过，然后收尾。 */
	void Cancel(const FString& InReason);

	/** 进行中（或刚结束）的报告，供 `job` 轮询。 */
	const FGameTestReport& GetReport() const
	{
		return Report;
	}

	bool HasReport() const
	{
		return bHasReport;
	}

	/** 本次 run 主流程的叶子数（心跳日志、进度估算；循环只按一轮算）。 */
	int32 GetQueueLength() const
	{
		return Flow.IsValid() ? Flow->CountLeaves() : 0;
	}

	/** 正在跑的脚本流程（调试、gt.job 的 at / guards）；没有 run 时为空。 */
	FGameTestFlow* GetFlow() const
	{
		return Flow.Get();
	}

	//~ 「刚碰过的控件」——给视口高亮用 ------------------------------------------
	// 执行器只负责**记录事实**（哪一步、碰了谁、成没成），画不画、画在哪由订阅方决定：
	// 执行器是算法层，不该知道 Slate 或视口的存在。

	/** 每完成一步 +1；订阅方比对它就知道要不要重画。 */
	int64 GetTouchSerial() const
	{
		return TouchSerial;
	}

	TWeakObjectPtr<UWidget> GetTouchedWidget() const
	{
		return TouchedWidget;
	}

	/** 刚碰过的控件在屏幕上的矩形（快照坐标系）；没有几何信息时为 false。 */
	bool GetTouchedRect(FVector2D& OutMin, FVector2D& OutMax) const
	{
		OutMin = TouchedMin;
		OutMax = TouchedMax;
		return bTouchedHasRect;
	}

	bool WasTouchSuccessful() const
	{
		return bTouchedSucceeded;
	}

	/** 正在执行（跨帧等待中）的那一步；两步之间或没有 run 时为空。阻挡界面处理据此避开这一步要点的界面。 */
	const FGameTestStep* GetStepInFlight() const
	{
		return bActive && bStepInFlight ? &CurrentRecord.Step : nullptr;
	}

	/**
	 * 记一条 run 期间发生的事（例如自动关掉了一个阻挡界面）：攒着，下一步结束时写进它的 `payload.<InKey>` 数组。
	 * 两步之间发生的就记在下一步上。执行器只管归档，不管事件内容。
	 */
	void AddStepEvent(const FString& InKey, const TSharedPtr<FJsonObject>& InEvent);

private:
	void StartCurrentStep(double InNowSeconds);
	/** 真去执行这一步（调动作）：没有跳过条件时开步就调，有的话等跳过判断说「没达成」再调。 */
	void RunHandler(double InNowSeconds);
	void CompleteCurrentStep(EGameTestStepStatus InStatus, const FString& InMessage, double InNowSeconds);

	//~ 已达成就跳过（1.6.0，GameTestSkipIf）
	/** 跳过判断走一帧：跳过就记 already 并连带跳过 also 步，没达成就 RunHandler，还在等就看有没有超时。 */
	void TickSkipGate(double InNowSeconds);
	/** 这一步做成功了：带跳过条件且要核对的，先等条件成立（TickVerify）再算通过；否则直接通过。 */
	void FinishSucceeded(const FString& InMessage, double InNowSeconds);
	void TickVerify(double InNowSeconds);
	/** 跳过的那一步之后连带跳过 InAlso 步（记 already，「随第 N 步跳过」）。 */
	void SkipAlong(int32 InAlso, const FGameTestStep& InLeader, double InNowSeconds);

	/** 把这一步解析到的目标记进「刚碰过的控件」（没有目标就清掉）。 */
	void RecordTouch(const FGameTestStepContext& InContext);

	void FinishRun();
	/** 报告的 outcome / blockedStep / blockedReason（2.8.0）。 */
	void FillOutcome(bool bInBlocked);

	/** 守护打断了正在等的这一步：收尾、不记录，守护处理完由 Flow 决定重做、跳过还是失败。 */
	void AbortStepForGuard();

	FGameTestReport Report;
	/** 脚本的执行（1.9.0）：下一步跑什么由它说了算，Runner 只跑叶子。 */
	TUniquePtr<FGameTestFlow> Flow;
	/** Flow 交出来、还没开始的叶子（UI 间隔、步间隔推迟开步时先存着）。 */
	FGameTestFlowLeaf NextLeaf;
	bool bHaveNextLeaf = false;
	/** 正在跑的叶子开步时的步骤（跳过判断会改 CurrentRecord.Step 的超时，原值从这里取）。 */
	FGameTestStep CurrentStep;
	FGameTestStepRecord CurrentRecord;
	bool bActive = false;
	bool bStepInFlight = false;
	bool bHasReport = false;
	double StepDeadline = 0.0;

	/** 下一步最早能开始的时刻（`StepDelayMs`）；只在"上一步成功且还有下一步"时被推后。 */
	double NextStepNotBefore = 0.0;

	/** 本次 run 的起跑时刻（`FPlatformTime::Seconds()`），用来算报告的墙钟耗时。 */
	double StartedAtSeconds = 0.0;

	/** 刚碰过的控件与它的成败（高亮层读）。 */
	TWeakObjectPtr<UWidget> TouchedWidget;
	bool bTouchedHasRect = false;
	FVector2D TouchedMin = FVector2D::ZeroVector;
	FVector2D TouchedMax = FVector2D::ZeroVector;
	bool bTouchedSucceeded = false;
	int64 TouchSerial = 0;

	TFunction<bool(double)> PendingPredicate;

	/** 当前跨帧步骤的进度描述（谓词每帧更新，超时消息里拼出去）。 */
	TSharedPtr<FString> PendingProgress;

	/** 当前跨帧步骤的提前失败原因（谓词写，非空即判失败）。 */
	TSharedPtr<FString> PendingFailure;

	/** 当前跨帧步骤结束时的收尾（见 FGameTestStepContext::PendingCleanup）。 */
	TFunction<void()> PendingCleanup;

	FGameTestOnFinished OnFinished;

	FString OutputDir;

	/** 本次 run 的节奏参数（由请求注入，不再每步读设置）。 */
	int32 DefaultStepTimeoutMs = 60000;
	int32 StepDelayMs = 0;

	TMap<FString, TWeakObjectPtr<UWidget>>* Handles = nullptr;
	TArray<TStrongObjectPtr<UObject>>* RootedObjects = nullptr;

	/** 见 FGameTestRuntimeState::OnStepFailed。 */
	TFunction<void(FGameTestStepRecord&, const FString&)> OnStepFailed;

	/** 正在取消：这时被标失败的步骤是人叫停的，不是测试没过，不留证据。 */
	bool bCancelling = false;

	int32 UiWaitTimeoutMs = 0;
	FString ClickVia;
	TSharedPtr<FJsonObject> MoveDefaults;

	/** UI 操作间隔（毫秒），见 FGameTestRunRequest::UiStepGapMs。 */
	int32 UiStepGapMs = 0;

	/** 上一个 UI 操作结束的时刻；还没做过 UI 操作时 < 0。 */
	double LastUiStepEndedAt = -1.0;

	/** 下一步是 UI 操作、离上一个 UI 操作还不到 UiStepGapMs 时，把 NextStepNotBefore 推后并返回 true。 */
	bool HoldForUiGap(double InNowSeconds);

	/** AddStepEvent 攒下的事件：键 → 事件列表，下一步结束时并进它的 Payload。 */
	TMap<FString, TArray<TSharedPtr<FJsonValue>>> PendingStepEvents;

	/** 已达成就跳过的跑法（见 FGameTestRunRequest::SkipIf）。 */
	FGameTestSkipOptions SkipOptions;
	/** 正在判断这一步是否已达成（还没调动作）；判断完清掉。 */
	TSharedPtr<FGameTestSkipGate> SkipGate;
	/** 判断的结论（「没达成…，照常做」），进步骤结果的 payload.skipCheck。 */
	FString SkipGateNote;
	/** 做完了、正在核对条件成立（verify）。 */
	bool bVerifying = false;
	TSharedPtr<FJsonObject> VerifySkipIf;
	FString VerifyMessage;
	int32 VerifyTimeoutMs = 0;
	double LastVerifyAt = -1.0;
};
