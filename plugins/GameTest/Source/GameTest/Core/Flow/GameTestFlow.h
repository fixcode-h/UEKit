// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Flow/Blocks/GameTestBlock.h"
#include "Core/Flow/GameTestDebugOptions.h"
#include "Core/Flow/GameTestGuards.h"
#include "Core/Flow/GameTestNode.h"
#include "Core/GameTestTypes.h"

class FGameTestFlowContext;
class FJsonObject;

/** 执行器问「下一步做什么」的回答。 */
enum class EGameTestFlowNext : uint8
{
	/** 跑一个叶子（`FGameTestFlowLeaf`）。 */
	Leaf,
	/** 这一帧没事做（块在稳住条件、守护在稳住、暂停中）：下一帧再问。 */
	Wait,
	/** 流程走完了（成功、失败、return 到顶都算），看 HasFailed。 */
	Done,
};

/** 交给执行器跑的叶子。 */
struct GAMETEST_API FGameTestFlowLeaf
{
	const FGameTestNode* Node = nullptr;
	/** 填值求过的步骤（`$var`、`=表达式` 都换成了值，`$item>Btn` 换成了句柄）。 */
	FGameTestStep Step;
	/** 填值求值失败：不调动作，直接记失败（原因在这里）。 */
	FString PreError;
	/** 在循环第几轮里（从 0，不在循环里 -1）。 */
	int32 Iter = -1;
};


/**
 * 脚本的执行（协议第 2、5 节）：一棵节点树 + 一个帧栈。块只回答「下一串跑什么」，
 * 等待、超时、onFail / retry、守护、断点、记录、推送都在这里统一做——加一种块不用改它。
 *
 * 和 FGameTestRunner 的分工：Runner 照旧跑叶子（调动作、等谓词、跳过判断、留证据、UI 间隔）；
 * 每跑完一个叶子告诉这里（OnLeafFinished），再问下一个（Next）。块的记录、禁用的步骤、跳过的步骤由这里直接写进报告。
 *
 * 只在游戏线程上用。上下文（FGameTestFlowContext）归它持有，一次 run 里地址不变（跨帧接口捕获了它的地址）。
 */
class GAMETEST_API FGameTestFlow
{
public:
	/**
	 * `InContext` 已经填好环境（世界、句柄表、场景根、日志序号…）；这里压入主脚本的作用域并填 params / vars / objects。
	 * `InParams`：gt.run 的 params。`InGlobalGuards`：运行配置的守护。`InReport` 是执行器的报告，块记录直接写进去。
	 */
	FGameTestFlow(const TSharedRef<const FGameTestScript>& InScript, TUniquePtr<FGameTestFlowContext>&& InContext,
		const TSharedPtr<FJsonObject>& InParams, const TArray<TSharedRef<FGameTestGuardDef>>& InGlobalGuards,
		const FGameTestDebugOptions& InDebug, FGameTestReport& InReport);
	~FGameTestFlow();

	/** 开局失败（参数、变量初值取不到）：为空表示可以跑。 */
	const FString& GetStartError() const { return StartError; }

	/**
	 * 下一步。一次调用里会连着做掉不用等的事（进块、块走一步、禁用的步骤），直到要跑叶子、要等、或结束。
	 * 每步开始前的守护也在这里查：有要处理的就先跑它的 do。
	 */
	EGameTestFlowNext Next(double InNowSeconds, FGameTestFlowLeaf& OutLeaf);

	/**
	 * 叶子跑完了（Runner 已经把记录写进报告）：处理 onFail（continue / retry / 往外传）、守护 do 的结果、expectNoErrors。
	 * `InOutRecord` 是刚写进报告的那条，expectNoErrors 失败时会被改成失败。
	 */
	void OnLeafFinished(FGameTestStepRecord& InOutRecord, double InNowSeconds);

	/** skipIf 的 also：当前这串里接下来 N 个步骤记「随这一步跳过」。 */
	void SkipAlong(int32 InCount, const FGameTestStep& InLeader, double InNowSeconds);

	/**
	 * 等待中的守护（每 0.2 秒）：`InInFlight` 是正在等的那一步。有守护要处理时返回 true——
	 * Runner 要中止这一步（收尾、不记录），接着问 Next，守护处理完按 after 重做 / 跳过 / 失败。
	 */
	bool PollGuardsWhileWaiting(const FGameTestStep& InInFlight, double InNowSeconds);

	/** run 被取消或失败收尾：没跑的步骤记跳过、还开着的块记失败并收拾作用域。 */
	void Abort(const FString& InReason, double InNowSeconds);

	/**
	 * 转去跑收尾段（2.7.0，场景的 teardown）：还没跑的步骤记跳过、开着的块收掉，再压入收尾段。
	 * 没有收尾段、已经在跑或跑过时返回 false。主流程跑完、失败和第一次按停止都走这里。
	 */
	bool BeginTeardown(const FString& InReason, double InNowSeconds);
	bool IsInTeardown() const { return bInTeardown; }

	bool IsFinished() const { return bFinished; }
	bool HasFailed() const { return bFailed; }
	/** 失败发生在执行前（setup）里（2.8.0）：结论记 blocked「前置不满足」。 */
	bool IsBlocked() const { return bBlocked; }
	/** 现在跑到哪一段：setup / steps / teardown（gt.job 的 phase）。 */
	const TCHAR* GetPhase() const;
	/**
	 * 三段各自的进度（2.8.0，gt.job 的 phases）：`{setup: {done, total}, steps: …, teardown: …}`。
	 * 只数这一段顶层的节点（块算一步，call 进去的子脚本步骤不算），重做、循环重复的同一步只算一次，
	 * 所以不会出现「第 9/8 步」。
	 */
	TSharedPtr<FJsonObject> PhaseCounts() const;

	//~ 调试 ---------------------------------------------------------------
	void Pause();
	void Resume();
	/** 单步：执行下一个节点后暂停。`bInto`：块也进去（停在块里的第一步）；否则块算一个节点。 */
	void Step(bool bInto);
	void SetBreakpoints(const TSet<FString>& InKeys) { Debug.Breakpoints = InKeys; }
	bool IsPaused() const { return bPaused; }

	/** 当前所在的步骤（id 或路径），gt.job 的 `at`。 */
	const FString& GetAt() const { return AtKey; }
	const FString& GetAtPath() const { return AtPath; }

	/** `{id: 次数}`（gt.job、报告）。 */
	TSharedPtr<FJsonObject> GuardCounts() const { return Monitor.CountsToJson(); }

	/** 主流程叶子数（进度估算）。 */
	int32 CountLeaves() const;

	FGameTestFlowContext& GetContext() { return *Context; }

	/** `gt.flow` 推送（子系统接到推送通道上）；没接时不推。 */
	TFunction<void(const TSharedRef<FJsonObject>&)> OnFlowEvent;

	/** 往下一条记录的 payload 里加一条（守护处理记录走它，和 Runner 的 AddStepEvent 一样）。 */
	TFunction<void(const FString&, const TSharedPtr<FJsonObject>&)> OnStepEvent;

private:
	enum class EFrameKind : uint8
	{
		List,
		Block,
		Guard,
	};

	struct FFrame
	{
		EFrameKind Kind = EFrameKind::List;
		//~ List
		const FGameTestNodeList* List = nullptr;
		int32 Index = 0;
		//~ Block
		const FGameTestNode* Node = nullptr;
		TSharedPtr<FGameTestBlockState> State;
		double StartedAt = 0.0;
		double Deadline = 0.0;
		int32 Attempt = 0;
		int32 Iter = -1;
		uint64 ErrorSeq = 0;
		uint64 LuaErrorSeq = 0;
		TSharedPtr<FJsonObject> Payload;
		FString PendingMessage;
		bool bNeedTick = true;
		//~ Guard
		TSharedPtr<FGameTestGuardDef> Guard;
		/** 守护打断了一个叶子：处理完按 after 处理它（重做时把那串的下标退回去）。 */
		const FGameTestNode* Interrupted = nullptr;
		int32 InterruptedFrame = INDEX_NONE;
		/** 打断的是一个在等的块（if 在稳住）：after=skip / fail 对这个块做。 */
		bool bBlockInterrupted = false;
		FString At;
		int32 StepsBefore = 0;
		bool bDoFailed = false;
		FString DoError;
	};

	//~ 帧
	void PushList(const FGameTestNodeList& InList);
	void EnterBlock(const FGameTestNode& InNode, int32 InAttempt, double InNowSeconds);
	/** 块结束：记一条块记录、推 leave、调 Leave、弹出。 */
	void LeaveBlock(int32 InFrame, EGameTestStepStatus InStatus, const FString& InMessage, double InNowSeconds);
	/** 失败往外传：从 InFrame 往外找块，按块的 onFail 处理（retry 重进、continue 吞掉、stop 继续往外）。 */
	void PropagateFailure(int32 InFrame, const FString& InMessage, double InNowSeconds, bool bAllowPause = true);
	void PropagateSignal(EGameTestFlowSignal InSignal, const FString& InMessage, double InNowSeconds);
	/** 弹到只剩 InKeep 帧（被弹的块记 InStatus，调 Leave）。 */
	/** 还开着的块记失败（调 Leave 收拾作用域），没轮到的步骤记跳过 */
	void UnwindAll(const FString& InReason, double InNowSeconds);
	void UnwindTo(int32 InKeep, EGameTestStepStatus InStatus, const FString& InMessage, double InNowSeconds);
	void TickBlock(int32 InFrame, double InNowSeconds, bool& bOutWait);
	void FinishGuard(int32 InFrame, double InNowSeconds);
	/** 整个 run 失败（流程层面的：守护处理不掉、信号没人接…）：记一条失败记录，结束。 */
	void Fail(const FString& InMessage, double InNowSeconds);

	//~ 叶子和守护
	bool PrepareLeaf(const FGameTestNode& InNode, FGameTestFlowLeaf& OutLeaf);
	/** 生效的守护（块内层 → 脚本 → 全局）。 */
	void CollectGuards(TArray<TSharedRef<FGameTestGuardDef>>& OutGuards) const;
	bool InGuard() const;
	void StartGuard(const TSharedRef<FGameTestGuardDef>& InGuard, const TCHAR* InAt, const FGameTestNode* InInterrupted, int32 InInterruptedFrame, bool bInBlock, double InNowSeconds);
	/** Poll 一遍守护，有要处理的就开始处理。`bOutPending`：有守护在稳住。 */
	bool TryStartGuard(bool bWaiting, const TArray<FString>& InTargets, const FGameTestNode* InInterrupted, int32 InInterruptedFrame, bool bInBlock, double InNowSeconds, bool& bOutPending);
	int32 CurrentIter() const;
	/** 开着的 call 块，外层到内层（记录的 calls）。 */
	TArray<FString> CurrentCalls() const;
	int32 ListDepth() const;

	//~ 记录、推送
	void AddRecord(const FGameTestNode& InNode, EGameTestStepStatus InStatus, const FString& InMessage, double InStartedAt, double InNowSeconds, const TSharedPtr<FJsonObject>& InPayload, bool bInBlock);
	/** 报告 `payload.vars`（1.11.2）：这一步用到、写到的变量此刻的值并进 payload。 */
	TSharedPtr<FJsonObject> WithStepVars(const FGameTestNode& InNode, EGameTestStepStatus InStatus, const TSharedPtr<FJsonObject>& InPayload) const;
	void Emit(const TCHAR* InPhase, const FGameTestNode* InNode, const TCHAR* InStatus = nullptr, int32 InIter = -1, const FString& InGuard = FString(), double InMs = -1.0);
	void SetAt(const FGameTestNode& InNode);
	/** 断点、单步、to、from：要不要在这个节点前停下。 */
	bool ShouldPauseAt(const FGameTestNode& InNode);
	void PauseAt(const FGameTestNode& InNode);

	/** 这个节点的 expectNoErrors 期间有没有新报错（日志 Error / Lua 报错）。 */
	bool CheckNoErrors(uint64 InErrorSeq, uint64 InLuaErrorSeq, FString& OutError) const;

	TSharedRef<const FGameTestScript> Script;
	TUniquePtr<FGameTestFlowContext> Context;
	TArray<TSharedRef<FGameTestGuardDef>> GlobalGuards;
	FGameTestDebugOptions Debug;
	FGameTestReport& Report;
	FGameTestGuardMonitor Monitor;
	TArray<FFrame> Frames;

	FString StartError;
	bool bFinished = false;
	bool bFailed = false;
	/** 正在跑收尾段：这里的失败只记警告，不改整次 run 的结论，也不再转一次收尾 */
	bool bInTeardown = false;
	/** 执行前（setup）的那串已经跑完；没有 setup 时一开始就是 true。 */
	bool bSetupDone = true;
	bool bBlocked = false;
	bool bPaused = false;
	/** gt.debug pause：下一个节点前停。 */
	bool bPauseRequested = false;
	bool bSeeking = false;
	/** 单步：停在下一个深度不超过它的节点前（INT32_MAX = 进块）。-1 = 没在单步。 */
	int32 StepDepth = -1;
	/** 刚从暂停里继续：同一个节点不再停一次。 */
	const FGameTestNode* ResumedAt = nullptr;
	/** pauseOnFail：暂停后继续时再往外传的失败。 */
	bool bFailurePending = false;
	FString PendingFailureMessage;
	int32 PendingFailureFrame = INDEX_NONE;

	/** 正在跑的叶子（Next 交出去、OnLeafFinished 之前）。 */
	const FGameTestNode* LeafInFlight = nullptr;
	int32 LeafFrame = INDEX_NONE;
	int32 LeafAttempt = 0;
	uint64 LeafErrorSeq = 0;
	uint64 LeafLuaErrorSeq = 0;
	/** 重做的叶子：下一次 Next 不再查断点。 */
	const FGameTestNode* RetryLeaf = nullptr;
	int32 RetryAttempt = 0;

	FString AtKey;
	FString AtPath;
};
