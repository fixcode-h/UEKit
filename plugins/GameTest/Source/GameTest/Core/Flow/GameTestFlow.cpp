// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Flow/GameTestFlow.h"
#include "Core/GameTestHandlerShared.h"

#include "Core/Flow/GameTestLeafRefs.h"
#include "Core/Flow/GameTestStepVars.h"
#include "Core/GameTestLogTap.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTestCompat.h"
#include "Observe/Lua/GameTestLuaBridge.h"

namespace
{
	/** 一次 Next 里最多连着处理多少件不用等的事：写错的块（每次都 Done 却不前进）不能把编辑器卡死。 */
	constexpr int32 MaxWorkPerNext = 512;

	const TCHAR* StatusWord(EGameTestStepStatus InStatus)
	{
		switch (InStatus)
		{
		case EGameTestStepStatus::Succeeded: return TEXT("passed");
		case EGameTestStepStatus::Failed: return TEXT("failed");
		case EGameTestStepStatus::Already: return TEXT("already");
		default: return TEXT("skipped");
		}
	}

	/** 守护 unlessStepTargets 要比对的：这一步的 target、toTarget 和 obj.* 的 obj（都换成内部的 @@ref: 写法）。 */
	void StepTargets(const FGameTestStep& InStep, TArray<FString>& OutTargets)
	{
		if (!InStep.Target.IsEmpty())
		{
			OutTargets.Add(InStep.Target);
		}
		if (!InStep.Params.IsValid())
		{
			return;
		}
		for (const TCHAR* Field : { TEXT("toTarget"), TEXT("obj") })
		{
			FString Text;
			FString Error;
			if (InStep.Params->HasField(Field) && GameTestHandlers::TargetText(*InStep.Params, Field, Text, Error) && !Text.IsEmpty())
			{
				OutTargets.Add(Text);
			}
		}
	}

	/** 动作中途不插守护（协议第 5 节）：拖拽、长按、走路进行中插一脚会把手势打断。 */
	bool IsUninterruptible(const FGameTestStep& InStep)
	{
		static const FName Drag(TEXT("drag"));
		static const FName Move(TEXT("move"));
		return InStep.Action == Drag || InStep.Action == Move || (InStep.Params.IsValid() && InStep.Params->HasField(TEXT("holdMs")));
	}

	void MergePayload(TSharedPtr<FJsonObject>& InOutTarget, const TSharedPtr<FJsonObject>& InFrom)
	{
		if (!InFrom.IsValid())
		{
			return;
		}
		if (!InOutTarget.IsValid())
		{
			InOutTarget = MakeShared<FJsonObject>();
		}
		for (const auto& Pair : InFrom->Values)
		{
			InOutTarget->SetField(GAMETEST_JSON_KEY(Pair.Key), Pair.Value);
		}
	}
}

FGameTestFlow::FGameTestFlow(const TSharedRef<const FGameTestScript>& InScript, TUniquePtr<FGameTestFlowContext>&& InContext, const TSharedPtr<FJsonObject>& InParams, const TArray<TSharedRef<FGameTestGuardDef>>& InGlobalGuards, const FGameTestDebugOptions& InDebug, FGameTestReport& InReport)
	: Script(InScript)
	, Context(MoveTemp(InContext))
	, GlobalGuards(InGlobalGuards)
	, Debug(InDebug)
	, Report(InReport)
{
	check(Context.IsValid());
	Monitor.Reset();
	if (!Context->PushScript(Script->Path))
	{
		StartError = TEXT("脚本作用域压不进去（maxCallDepth 设成了 0？）");
		bFinished = true;
		bFailed = true;
		return;
	}
	FGameTestScriptScope& Scope = Context->GetScope();
	for (const FGameTestParamDef& Param : Script->Params)
	{
		const TSharedPtr<FJsonValue> Given = InParams.IsValid() ? InParams->TryGetField(Param.Name) : nullptr;
		Scope.Params.Add(Param.Name, FGameTestValue::FromJson(Given.IsValid() ? Given : Param.Default));
	}
	if (InParams.IsValid())
	{
		for (const auto& Pair : InParams->Values)
		{
			const FString Key = GAMETEST_JSON_KEY(Pair.Key);
			if (!Scope.Params.Contains(Key))
			{
				// 场景没声明的参数也收下（临时步骤没有 params 段，Agent 直接传）。
				Scope.Params.Add(Key, FGameTestValue::FromJson(Pair.Value));
			}
		}
	}
	if (Script->Objects.IsValid())
	{
		Scope.AddObjects(*Script->Objects);
	}
	if (Script->Vars.IsValid())
	{
		for (const auto& Pair : Script->Vars->Values)
		{
			FGameTestValue Value = GameTestRefs::EvalJson(Pair.Value, *Context);
			if (Value.IsUnresolved())
			{
				StartError = FString::Printf(TEXT("变量 %s 的初值取不到：%s"), *GAMETEST_JSON_KEY(Pair.Key), *Value.GetString());
				bFinished = true;
				bFailed = true;
				return;
			}
			Context->GetScope().Vars.Add(GAMETEST_JSON_KEY(Pair.Key), MoveTemp(Value));
		}
	}
	if (!Debug.From.IsEmpty())
	{
		if (!Script->FindNode(Debug.From))
		{
			StartError = FString::Printf(TEXT("from 写的步骤 %s 在场景里找不到（写步骤 id 或路径）"), *Debug.From);
			bFinished = true;
			bFailed = true;
			return;
		}
		bSeeking = true;
	}
	// 栈顶先跑：setup 压在 steps 上面。
	PushList(Script->Steps);
	if (Script->Setup.Nodes.Num() > 0)
	{
		PushList(Script->Setup);
	}
}

FGameTestFlow::~FGameTestFlow() = default;

int32 FGameTestFlow::CountLeaves() const
{
	return Script->CountLeaves();
}

//~ 主循环 -------------------------------------------------------------------

EGameTestFlowNext FGameTestFlow::Next(double InNowSeconds, FGameTestFlowLeaf& OutLeaf)
{
	Context->NowSeconds = InNowSeconds;
	if (bFailurePending && !bPaused)
	{
		// pauseOnFail 暂停过、人点了继续：这时才按 onFail 往外传。
		bFailurePending = false;
		PropagateFailure(PendingFailureFrame, PendingFailureMessage, InNowSeconds, false);
	}
	for (int32 Work = 0; Work < MaxWorkPerNext; ++Work)
	{
		if (bFinished)
		{
			return EGameTestFlowNext::Done;
		}
		if (bPaused)
		{
			return EGameTestFlowNext::Wait;
		}
		if (Frames.Num() == 0)
		{
			bFinished = true;
			return EGameTestFlowNext::Done;
		}
		const int32 Top = Frames.Num() - 1;
		if (Frames[Top].Kind == EFrameKind::Block)
		{
			bool bWait = false;
			TickBlock(Top, InNowSeconds, bWait);
			if (bWait)
			{
				return EGameTestFlowNext::Wait;
			}
			continue;
		}
		if (Frames[Top].Kind == EFrameKind::Guard)
		{
			FinishGuard(Top, InNowSeconds);
			continue;
		}

		// 列表帧：跑下一个节点
		FFrame& Frame = Frames[Top];
		if (Frame.Index >= Frame.List->Nodes.Num())
		{
			Frames.Pop();
			if (Frames.Num() > 0 && Frames.Last().Kind == EFrameKind::Block)
			{
				Frames.Last().bNeedTick = true;
			}
			continue;
		}
		const FGameTestNode& Node = *Frame.List->Nodes[Frame.Index];
		const bool bRetry = RetryLeaf == &Node;

		if (bSeeking && Node.Path.StartsWith(TEXT("steps")))
		{
			if (Node.Id == Debug.From || Node.Path == Debug.From)
			{
				bSeeking = false;
			}
			else
			{
				++Frame.Index;
				if (Node.Contains(Debug.From))
				{
					// from 的外层块：不进块本身（条件、轮次都不判），只走包含 from 的那一串一次。
					for (const FGameTestNodeList& List : Node.Lists)
					{
						bool bHas = false;
						for (const TSharedRef<FGameTestNode>& Child : List.Nodes)
						{
							bHas = bHas || Child->Id == Debug.From || Child->Path == Debug.From || Child->Contains(Debug.From);
						}
						if (bHas)
						{
							PushList(List);
							break;
						}
					}
				}
				continue;
			}
		}

		if (!bRetry && !InGuard())
		{
			TArray<FString> Targets;
			if (!Node.IsBlock())
			{
				StepTargets(Node.Step, Targets);
			}
			bool bPending = false;
			if (TryStartGuard(false, Targets, nullptr, Top, false, InNowSeconds, bPending))
			{
				continue;
			}
			if (bPending)
			{
				// 守护的界面已经出来、在稳住：等它稳住处理掉再开这一步，不然这一步会被它挡住。
				return EGameTestFlowNext::Wait;
			}
		}

		if (ResumedAt != &Node && !bRetry && ShouldPauseAt(Node))
		{
			PauseAt(Node);
			return EGameTestFlowNext::Wait;
		}
		ResumedAt = nullptr;
		// 下面会压帧，Frame 引用可能失效：先前进下标。
		Frames[Top].Index++;

		if (Node.bDisabled)
		{
			AddRecord(Node, EGameTestStepStatus::Disabled, TEXT("disabled：没做"), InNowSeconds, InNowSeconds, nullptr, Node.IsBlock());
			Emit(TEXT("leave"), &Node, TEXT("skipped"));
			continue;
		}
		SetAt(Node);
		if (Node.IsBlock())
		{
			EnterBlock(Node, 0, InNowSeconds);
			continue;
		}

		LeafInFlight = &Node;
		LeafFrame = Top;
		LeafAttempt = bRetry ? RetryAttempt : 0;
		RetryLeaf = nullptr;
		LeafErrorSeq = Context->LogTap ? Context->LogTap->GetErrorSeq() : 0;
		LeafLuaErrorSeq = GameTestLuaBridge::ErrorSeq();
		OutLeaf = FGameTestFlowLeaf();
		OutLeaf.Node = &Node;
		OutLeaf.Iter = CurrentIter();
		PrepareLeaf(Node, OutLeaf);
		Emit(TEXT("enter"), &Node);
		return EGameTestFlowNext::Leaf;
	}
	return EGameTestFlowNext::Wait;
}

bool FGameTestFlow::PrepareLeaf(const FGameTestNode& InNode, FGameTestFlowLeaf& OutLeaf)
{
	OutLeaf.Step = InNode.Step;
	FString Error;
	if (!GameTestLeafRefs::Resolve(OutLeaf.Step, *Context, Error))
	{
		OutLeaf.PreError = Error;
		return false;
	}
	return true;
}

void FGameTestFlow::OnLeafFinished(FGameTestStepRecord& InOutRecord, double InNowSeconds)
{
	const FGameTestNode* Node = LeafInFlight;
	LeafInFlight = nullptr;
	if (!Node)
	{
		return;
	}
	if (Node->bExpectNoErrors && InOutRecord.Status != EGameTestStepStatus::Failed)
	{
		FString Error;
		if (!CheckNoErrors(LeafErrorSeq, LeafLuaErrorSeq, Error))
		{
			InOutRecord.Status = EGameTestStepStatus::Failed;
			InOutRecord.Message = FString::Printf(TEXT("%s；%s"), *InOutRecord.Message, *Error);
		}
	}
	Emit(TEXT("leave"), Node, StatusWord(InOutRecord.Status), -1, FString(), (InOutRecord.FinishedAt - InOutRecord.StartedAt) * 1000.0);
	InOutRecord.bWarn = Node->bHookNote && InOutRecord.Status == EGameTestStepStatus::Failed;
	if (InOutRecord.Status != EGameTestStepStatus::Failed || bFinished)
	{
		return;
	}
	const FGameTestStep& Step = Node->Step;
	if (Step.FailMode == EGameTestFailMode::Retry && LeafAttempt < Node->Retries && Frames.IsValidIndex(LeafFrame))
	{
		// 把那串的下标退回这一步，下一次 Next 重做它。
		Frames[LeafFrame].Index = FMath::Max(0, Frames[LeafFrame].Index - 1);
		RetryLeaf = Node;
		RetryAttempt = LeafAttempt + 1;
		InOutRecord.Message = FString::Printf(TEXT("%s；重做第 %d 次（共 %d 次）"), *InOutRecord.Message, RetryAttempt, Node->Retries);
		return;
	}
	if (Step.FailMode == EGameTestFailMode::Continue)
	{
		return;
	}
	PropagateFailure(LeafFrame, InOutRecord.Message, InNowSeconds);
}

void FGameTestFlow::SkipAlong(int32 InCount, const FGameTestStep& InLeader, double InNowSeconds)
{
	if (!Frames.IsValidIndex(LeafFrame) || Frames[LeafFrame].Kind != EFrameKind::List)
	{
		return;
	}
	// 记录在循环里把 Frames[LeafFrame] 的下标往后推；AddRecord 不压帧，引用不会失效。
	FFrame& Frame = Frames[LeafFrame];
	for (int32 Count = 0; Count < InCount && Frame.Index < Frame.List->Nodes.Num(); ++Count)
	{
		const FGameTestNode& Node = *Frame.List->Nodes[Frame.Index++];
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Skip = MakeShared<FJsonObject>();
		Skip->SetStringField(TEXT("with"), InLeader.Id.IsEmpty() ? InLeader.Path : InLeader.Id);
		Skip->SetNumberField(TEXT("withIndex"), InLeader.Index);
		Payload->SetObjectField(TEXT("skip"), Skip);
		AddRecord(Node, EGameTestStepStatus::Already, FString::Printf(TEXT("随%s跳过（那一步已达成）"), *InLeader.Label()), InNowSeconds,
			InNowSeconds, Payload, Node.IsBlock());
		Emit(TEXT("leave"), &Node, TEXT("already"));
	}
}

//~ 块 -----------------------------------------------------------------------

void FGameTestFlow::PushList(const FGameTestNodeList& InList)
{
	FFrame Frame;
	Frame.Kind = EFrameKind::List;
	Frame.List = &InList;
	Frames.Add(MoveTemp(Frame));
}

void FGameTestFlow::EnterBlock(const FGameTestNode& InNode, int32 InAttempt, double InNowSeconds)
{
	FFrame Frame;
	Frame.Kind = EFrameKind::Block;
	Frame.Node = &InNode;
	Frame.State = InNode.Block->Enter(InNode, *Context);
	Frame.StartedAt = InNowSeconds;
	Frame.Deadline = InNode.Step.TimeoutMs > 0 ? InNowSeconds + InNode.Step.TimeoutMs / 1000.0 : 0.0;
	Frame.Attempt = InAttempt;
	Frame.ErrorSeq = Context->LogTap ? Context->LogTap->GetErrorSeq() : 0;
	Frame.LuaErrorSeq = GameTestLuaBridge::ErrorSeq();
	Frame.bNeedTick = true;
	Frames.Add(MoveTemp(Frame));
	Emit(TEXT("enter"), &InNode);
}

void FGameTestFlow::TickBlock(int32 InFrame, double InNowSeconds, bool& bOutWait)
{
	bOutWait = false;
	const FGameTestNode& Node = *Frames[InFrame].Node;
	if (Frames[InFrame].Deadline > 0.0 && InNowSeconds >= Frames[InFrame].Deadline)
	{
		const FString Waiting = Frames[InFrame].PendingMessage;
		PropagateFailure(InFrame, FString::Printf(TEXT("整块超时（timeoutMs=%d）%s"), Node.Step.TimeoutMs,
			Waiting.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("：%s"), *Waiting)), InNowSeconds);
		return;
	}

	FGameTestBlockTick Out;
	const EGameTestBlockStep Result = Node.Block->Tick(Node, *Frames[InFrame].State, *Context, InNowSeconds, Out);
	MergePayload(Frames[InFrame].Payload, Out.Payload);
	if (Out.Iter != INDEX_NONE)
	{
		Frames[InFrame].Iter = Out.Iter;
		// 推送的轮次从 1 数（面板直接显示）。
		Emit(TEXT("iter"), &Node, nullptr, Out.Iter + 1);
	}
	switch (Result)
	{
	case EGameTestBlockStep::Pending:
	{
		Frames[InFrame].PendingMessage = Out.Message;
		bOutWait = true;
		// 块在等（if 稳住条件、loop 隔 gapMs）也算等待中：照常查守护。
		if (!InGuard() && Monitor.IsWaitCheckDue(InNowSeconds))
		{
			Monitor.MarkWaitChecked(InNowSeconds);
			bool bPending = false;
			if (TryStartGuard(true, TArray<FString>(), nullptr, InFrame, true, InNowSeconds, bPending))
			{
				bOutWait = false;
			}
		}
		return;
	}
	case EGameTestBlockStep::RunList:
	{
		const FGameTestNodeList* List = Out.ExternalList ? Out.ExternalList : (Node.Lists.IsValidIndex(Out.List) ? &Node.Lists[Out.List] : nullptr);
		if (!List)
		{
			PropagateFailure(InFrame, FString::Printf(TEXT("%s 要跑的子步骤不存在（块的实现有问题）"), *Node.Action.ToString()), InNowSeconds);
			return;
		}
		Frames[InFrame].bNeedTick = false;
		Frames[InFrame].PendingMessage.Reset();
		PushList(*List);
		return;
	}
	case EGameTestBlockStep::Done:
	{
		FString Error;
		if (Node.bExpectNoErrors && !CheckNoErrors(Frames[InFrame].ErrorSeq, Frames[InFrame].LuaErrorSeq, Error))
		{
			PropagateFailure(InFrame, Error, InNowSeconds);
			return;
		}
		LeaveBlock(InFrame, EGameTestStepStatus::Succeeded, Out.Message, InNowSeconds);
		return;
	}
	case EGameTestBlockStep::Signal:
	{
		LeaveBlock(InFrame, EGameTestStepStatus::Succeeded, Out.Message, InNowSeconds);
		PropagateSignal(Out.Signal, Out.Message, InNowSeconds);
		return;
	}
	case EGameTestBlockStep::Failed:
	default:
		PropagateFailure(InFrame, Out.Message.IsEmpty() ? FString::Printf(TEXT("%s 失败"), *Node.Action.ToString()) : Out.Message, InNowSeconds);
		return;
	}
}

void FGameTestFlow::LeaveBlock(int32 InFrame, EGameTestStepStatus InStatus, const FString& InMessage, double InNowSeconds)
{
	// 上面的帧先收掉（调用方一般已经收了，这里兜底）。
	UnwindTo(InFrame + 1, EGameTestStepStatus::Failed, TEXT("没跑完"), InNowSeconds);
	if (!Frames.IsValidIndex(InFrame) || Frames[InFrame].Kind != EFrameKind::Block)
	{
		return;
	}
	FFrame Frame = MoveTemp(Frames[InFrame]);
	Frames.RemoveAt(InFrame);
	const FGameTestNode& Node = *Frame.Node;
	Node.Block->Leave(Node, *Frame.State, *Context);
	if (Frame.Attempt > 0)
	{
		if (!Frame.Payload.IsValid())
		{
			Frame.Payload = MakeShared<FJsonObject>();
		}
		Frame.Payload->SetNumberField(TEXT("attempt"), Frame.Attempt);
	}
	AddRecord(Node, InStatus, InMessage, Frame.StartedAt, InNowSeconds, Frame.Payload, true);
	Emit(TEXT("leave"), &Node, StatusWord(InStatus), -1, FString(), (InNowSeconds - Frame.StartedAt) * 1000.0);
}

void FGameTestFlow::UnwindTo(int32 InKeep, EGameTestStepStatus InStatus, const FString& InMessage, double InNowSeconds)
{
	while (Frames.Num() > FMath::Max(0, InKeep))
	{
		const int32 Top = Frames.Num() - 1;
		if (Frames[Top].Kind == EFrameKind::Block)
		{
			LeaveBlock(Top, InStatus, InMessage, InNowSeconds);
		}
		else
		{
			if (Frames[Top].Kind == EFrameKind::Guard)
			{
				Emit(TEXT("guardEnd"), nullptr, nullptr, -1, Frames[Top].Guard.IsValid() ? Frames[Top].Guard->Key() : FString());
			}
			Frames.Pop();
		}
	}
}

void FGameTestFlow::PropagateFailure(int32 InFrame, const FString& InMessage, double InNowSeconds, bool bAllowPause)
{
	if (bFinished)
	{
		return;
	}
	if (bAllowPause && Debug.bPauseOnFail)
	{
		// 先停下等人看现场；继续后（Resume → Next）再按 onFail 往外传。
		bFailurePending = true;
		PendingFailureFrame = InFrame;
		PendingFailureMessage = InMessage;
		bPaused = true;
		Emit(TEXT("pause"), nullptr);
		return;
	}
	int32 Index = FMath::Min(InFrame, Frames.Num() - 1);
	while (Index >= 0)
	{
		const EFrameKind Kind = Frames[Index].Kind;
		if (Kind == EFrameKind::Guard)
		{
			// 守护的 do 失败：收掉 do 里的帧，交给 FinishGuard 按守护的 onFail 处理。
			UnwindTo(Index + 1, EGameTestStepStatus::Failed, TEXT("守护处理失败"), InNowSeconds);
			Frames[Index].bDoFailed = true;
			Frames[Index].DoError = InMessage;
			return;
		}
		if (Kind == EFrameKind::Block)
		{
			const FGameTestNode& Node = *Frames[Index].Node;
			const int32 Attempt = Frames[Index].Attempt;
			UnwindTo(Index + 1, EGameTestStepStatus::Failed, TEXT("没跑完（里面的步骤失败）"), InNowSeconds);
			if (Node.Block->CatchFailure(Node, *Frames[Index].State, *Context, InMessage))
			{
				// 块接住了失败（附带 GM 要接着跑「失败时」那组）：由它的 Tick 决定后面怎么走。
				Frames[Index].bNeedTick = true;
				return;
			}
			if (Node.Step.FailMode == EGameTestFailMode::Retry && Attempt < Node.Retries)
			{
				// 整块重做：从块的第一步开始，状态全新。
				LeaveBlock(Index, EGameTestStepStatus::Failed, FString::Printf(TEXT("%s；重做第 %d 次（共 %d 次）"), *InMessage, Attempt + 1, Node.Retries),
					InNowSeconds);
				EnterBlock(Node, Attempt + 1, InNowSeconds);
				return;
			}
			LeaveBlock(Index, EGameTestStepStatus::Failed, InMessage, InNowSeconds);
			if (Node.Step.FailMode == EGameTestFailMode::Continue)
			{
				return;
			}
			Index = Frames.Num() - 1;
			continue;
		}
		--Index;
	}
	Fail(InMessage, InNowSeconds);
}

void FGameTestFlow::PropagateSignal(EGameTestFlowSignal InSignal, const FString& InMessage, double InNowSeconds)
{
	for (int32 Index = Frames.Num() - 1; Index >= 0; --Index)
	{
		if (Frames[Index].Kind == EFrameKind::Guard)
		{
			UnwindTo(Index + 1, EGameTestStepStatus::Failed, TEXT("没跑完"), InNowSeconds);
			Frames[Index].bDoFailed = true;
			Frames[Index].DoError = FString::Printf(TEXT("守护的 do 里不能用 %s（它跳不出守护）"), *InMessage);
			return;
		}
		if (Frames[Index].Kind == EFrameKind::Block
			&& Frames[Index].Node->Block->CatchSignal(InSignal, *Frames[Index].Node, *Frames[Index].State, *Context))
		{
			// 接住的块里面还开着的（break 所在的 if、group）收掉，算正常结束。
			UnwindTo(Index + 1, EGameTestStepStatus::Succeeded, FString::Printf(TEXT("%s 跳出"), *InMessage), InNowSeconds);
			Frames[Index].bNeedTick = true;
			return;
		}
	}
	if (InSignal == EGameTestFlowSignal::Return)
	{
		// 主脚本 return：后面的步骤不跑，算正常结束。
		UnwindTo(0, EGameTestStepStatus::Succeeded, TEXT("return 跳出"), InNowSeconds);
		bFinished = true;
		return;
	}
	Fail(TEXT("break / continue 不在循环里"), InNowSeconds);
}

void FGameTestFlow::Fail(const FString& InMessage, double InNowSeconds)
{
	if (bFinished)
	{
		return;
	}
	// 走到这里的失败不一定有一条失败的记录（守护处理不掉、信号没人接）：补一条，报告才不会显示成通过。
	bool bHasFailure = false;
	for (const FGameTestStepRecord& Record : Report.Steps)
	{
		bHasFailure = bHasFailure || Record.Status == EGameTestStepStatus::Failed;
	}
	if (!bHasFailure)
	{
		FGameTestStepRecord Record;
		Record.Step.Action = TEXT("flow");
		Record.Step.Id = AtKey;
		Record.Step.Path = AtPath;
		Record.Step.bNested = true;
		Record.Status = EGameTestStepStatus::Failed;
		Record.Message = InMessage;
		Record.StartedAt = InNowSeconds;
		Record.FinishedAt = InNowSeconds;
		Record.Iter = CurrentIter();
		Report.Steps.Add(MoveTemp(Record));
	}
	bFailed = true;
	bFinished = true;
}

void FGameTestFlow::Abort(const FString& InReason, double InNowSeconds)
{
	// 还开着的块记失败（调 Leave 收拾作用域），没轮到的步骤记跳过：报告里能看到哪些没跑。
	while (Frames.Num() > 0)
	{
		const int32 Top = Frames.Num() - 1;
		if (Frames[Top].Kind == EFrameKind::List)
		{
			FFrame& Frame = Frames[Top];
			for (int32 Index = Frame.Index; Index < Frame.List->Nodes.Num(); ++Index)
			{
				const FGameTestNode& Node = *Frame.List->Nodes[Index];
				FGameTestStepRecord Record;
				Record.Step = Node.Step;
				Record.Step.Id = Node.Id;
				Record.Step.Path = Node.Path;
				Record.bBlock = Node.IsBlock();
				Record.Status = EGameTestStepStatus::Skipped;
				Record.Message = InReason;
				Record.Iter = CurrentIter();
				Report.Steps.Add(MoveTemp(Record));
			}
			Frames.Pop();
			continue;
		}
		if (Frames[Top].Kind == EFrameKind::Block)
		{
			LeaveBlock(Top, EGameTestStepStatus::Failed, InReason, InNowSeconds);
			continue;
		}
		Frames.Pop();
	}
	LeafInFlight = nullptr;
	bFinished = true;
	if (Context.IsValid() && Context->GetScriptDepth() > 0)
	{
		Context->PopScript();
	}
}

//~ 守护 ---------------------------------------------------------------------

void FGameTestFlow::CollectGuards(TArray<TSharedRef<FGameTestGuardDef>>& OutGuards) const
{
	// 块内层 → 外层（同层按数组顺序），再脚本、全局。
	for (int32 Index = Frames.Num() - 1; Index >= 0; --Index)
	{
		const FFrame& Frame = Frames[Index];
		if (Frame.Kind != EFrameKind::Block)
		{
			continue;
		}
		OutGuards.Append(Frame.Node->Guards);
		Frame.Node->Block->CollectGuards(*Frame.Node, *Frame.State, OutGuards);
	}
	OutGuards.Append(Script->Guards);
	OutGuards.Append(GlobalGuards);
}

bool FGameTestFlow::InGuard() const
{
	for (const FFrame& Frame : Frames)
	{
		if (Frame.Kind == EFrameKind::Guard)
		{
			return true;
		}
	}
	return false;
}

bool FGameTestFlow::TryStartGuard(bool bWaiting, const TArray<FString>& InTargets, const FGameTestNode* InInterrupted, int32 InInterruptedFrame, bool bInBlock, double InNowSeconds, bool& bOutPending)
{
	bOutPending = false;
	TArray<TSharedRef<FGameTestGuardDef>> Active;
	CollectGuards(Active);
	if (Active.Num() == 0)
	{
		return false;
	}
	const FGameTestGuardDef* Hit = Monitor.Poll(Active, bWaiting, InTargets, *Context, InNowSeconds, bOutPending);
	if (!Hit)
	{
		return false;
	}
	const TSharedRef<FGameTestGuardDef>* Shared = Active.FindByPredicate([Hit](const TSharedRef<FGameTestGuardDef>& InGuard)
	{
		return &InGuard.Get() == Hit;
	});
	if (!Shared)
	{
		return false;
	}
	StartGuard(*Shared, bWaiting ? TEXT("waiting") : TEXT("beforeStep"), InInterrupted, InInterruptedFrame, bInBlock, InNowSeconds);
	return true;
}

void FGameTestFlow::StartGuard(const TSharedRef<FGameTestGuardDef>& InGuard, const TCHAR* InAt, const FGameTestNode* InInterrupted, int32 InInterruptedFrame, bool bInBlock, double InNowSeconds)
{
	FString Error;
	if (!Monitor.BeginHandle(*InGuard, InNowSeconds, Error))
	{
		Fail(Error, InNowSeconds);
		return;
	}
	FFrame Frame;
	Frame.Kind = EFrameKind::Guard;
	Frame.Guard = InGuard;
	Frame.At = InAt;
	Frame.Interrupted = InInterrupted;
	Frame.InterruptedFrame = InInterruptedFrame;
	Frame.bBlockInterrupted = bInBlock;
	Frame.StartedAt = InNowSeconds;
	Frame.StepsBefore = Report.Steps.Num();
	Frames.Add(MoveTemp(Frame));
	Emit(TEXT("guard"), nullptr, nullptr, -1, InGuard->Key());
	PushList(InGuard->Do);
}

bool FGameTestFlow::PollGuardsWhileWaiting(const FGameTestStep& InInFlight, double InNowSeconds)
{
	if (!LeafInFlight || InGuard() || bFinished || IsUninterruptible(InInFlight) || !Monitor.IsWaitCheckDue(InNowSeconds))
	{
		return false;
	}
	Monitor.MarkWaitChecked(InNowSeconds);
	Context->NowSeconds = InNowSeconds;
	TArray<FString> Targets;
	StepTargets(InInFlight, Targets);
	bool bPending = false;
	const FGameTestNode* Interrupted = LeafInFlight;
	const int32 Frame = LeafFrame;
	// 先登记守护再清「在跑的叶子」：StartGuard 失败（超过 max）时整个 run 结束，执行器同样要中止这一步。
	LeafInFlight = nullptr;
	if (TryStartGuard(true, Targets, Interrupted, Frame, false, InNowSeconds, bPending) || bFinished)
	{
		// 被打断的那一步要重做时用同一个重做次数。
		RetryAttempt = LeafAttempt;
		return true;
	}
	LeafInFlight = Interrupted;
	return false;
}

void FGameTestFlow::FinishGuard(int32 InFrame, double InNowSeconds)
{
	FFrame Frame = MoveTemp(Frames[InFrame]);
	Frames.RemoveAt(InFrame);
	const FGameTestGuardDef& Guard = *Frame.Guard;
	bool bOk = true;
	bool bStill = false;
	FString Message;
	if (Frame.bDoFailed)
	{
		Message = FString::Printf(TEXT("守护「%s」的 do 失败：%s"), *Guard.Label(), *Frame.DoError);
		bOk = Guard.bIgnoreFail;
	}
	FString Error;
	if (bOk && !Monitor.EndHandle(Guard, *Context, InNowSeconds, bStill, Error))
	{
		bOk = false;
		Message = Error;
	}

	TSharedPtr<FJsonObject> Event = MakeShared<FJsonObject>();
	Event->SetStringField(TEXT("id"), Guard.Key());
	if (!Guard.Name.IsEmpty())
	{
		Event->SetStringField(TEXT("name"), Guard.Name);
	}
	Event->SetStringField(TEXT("at"), Frame.At);
	Event->SetNumberField(TEXT("ms"), FMath::RoundToInt((InNowSeconds - Frame.StartedAt) * 1000.0));
	Event->SetNumberField(TEXT("steps"), Report.Steps.Num() - Frame.StepsBefore);
	Event->SetBoolField(TEXT("ok"), bOk && !Frame.bDoFailed);
	if (!Message.IsEmpty())
	{
		Event->SetStringField(TEXT("message"), Message);
	}
	if (bStill)
	{
		Event->SetBoolField(TEXT("stillThere"), true);
	}
	if (OnStepEvent)
	{
		OnStepEvent(TEXT("guards"), Event);
	}
	Emit(TEXT("guardEnd"), nullptr, bOk ? TEXT("passed") : TEXT("failed"), -1, Guard.Key(), (InNowSeconds - Frame.StartedAt) * 1000.0);

	if (!bOk)
	{
		Fail(Message, InNowSeconds);
		return;
	}

	// 被打断（或还没开始）的那一步按 after 处理。
	const bool bValidFrame = Frames.IsValidIndex(Frame.InterruptedFrame);
	if (Frame.bBlockInterrupted)
	{
		if (!bValidFrame || Frames[Frame.InterruptedFrame].Kind != EFrameKind::Block)
		{
			return;
		}
		if (Guard.After == EGameTestGuardAfter::Skip)
		{
			LeaveBlock(Frame.InterruptedFrame, EGameTestStepStatus::Already, FString::Printf(TEXT("守护「%s」处理后按 after=skip 跳过"), *Guard.Label()),
				InNowSeconds);
		}
		else if (Guard.After == EGameTestGuardAfter::Fail)
		{
			PropagateFailure(Frame.InterruptedFrame, FString::Printf(TEXT("守护「%s」出现，按 after=fail 算失败"), *Guard.Label()), InNowSeconds);
		}
		return;
	}
	if (!bValidFrame || Frames[Frame.InterruptedFrame].Kind != EFrameKind::List)
	{
		return;
	}
	FFrame& List = Frames[Frame.InterruptedFrame];
	// 等待中打断的：那一步已经算开始过（下标在它后面），退回去；每步开始前的：还没开始，下标就指着它。
	if (Frame.Interrupted)
	{
		List.Index = FMath::Max(0, List.Index - 1);
	}
	if (!List.List->Nodes.IsValidIndex(List.Index))
	{
		return;
	}
	const FGameTestNode& Node = *List.List->Nodes[List.Index];
	switch (Guard.After)
	{
	case EGameTestGuardAfter::Skip:
		List.Index++;
		AddRecord(Node, EGameTestStepStatus::Already, FString::Printf(TEXT("守护「%s」处理后按 after=skip 跳过"), *Guard.Label()), InNowSeconds,
			InNowSeconds, nullptr, Node.IsBlock());
		Emit(TEXT("leave"), &Node, TEXT("already"));
		break;
	case EGameTestGuardAfter::Fail:
	{
		List.Index++;
		const FString FailMessage = FString::Printf(TEXT("守护「%s」出现，按 after=fail 算失败"), *Guard.Label());
		AddRecord(Node, EGameTestStepStatus::Failed, FailMessage, InNowSeconds, InNowSeconds, nullptr, Node.IsBlock());
		Emit(TEXT("leave"), &Node, TEXT("failed"));
		PropagateFailure(Frame.InterruptedFrame, FailMessage, InNowSeconds);
		break;
	}
	case EGameTestGuardAfter::Retry:
	default:
		// 下标已经指着它：下一次 Next 重新开始这一步（重新计时）。等待中被打断的不再查断点。
		if (Frame.Interrupted)
		{
			RetryLeaf = Frame.Interrupted;
		}
		break;
	}
}

//~ 调试 ---------------------------------------------------------------------

void FGameTestFlow::Pause()
{
	bPauseRequested = true;
}

void FGameTestFlow::Resume()
{
	bPauseRequested = false;
	StepDepth = -1;
	if (bPaused)
	{
		bPaused = false;
		Emit(TEXT("resume"), nullptr);
	}
}

void FGameTestFlow::Step(bool bInto)
{
	if (!bPaused)
	{
		// 没停着时单步 = 下一个节点前停。
		bPauseRequested = true;
		return;
	}
	// 停着的那个节点做完后，停在下一个深度不超过它的节点前；into 时块里的第一步就停。
	StepDepth = bInto ? MAX_int32 : ListDepth();
	bPaused = false;
	Emit(TEXT("resume"), nullptr);
}

bool FGameTestFlow::ShouldPauseAt(const FGameTestNode& InNode)
{
	if (InNode.bHookWrap || InNode.bInHook)
	{
		// 附带 GM 不停：断点、单步、暂停请求都留给主步骤（暂停请求不清，下一个主步骤前停）。
		return false;
	}
	if (bPauseRequested)
	{
		bPauseRequested = false;
		return true;
	}
	if (StepDepth >= 0 && ListDepth() <= StepDepth)
	{
		return true;
	}
	if (Debug.Breakpoints.Contains(InNode.Id) || Debug.Breakpoints.Contains(InNode.Path))
	{
		return true;
	}
	if (!Debug.To.IsEmpty() && (InNode.Id == Debug.To || InNode.Path == Debug.To))
	{
		// to 只停一次：继续之后接着往下跑。
		Debug.To.Reset();
		return true;
	}
	return false;
}

void FGameTestFlow::PauseAt(const FGameTestNode& InNode)
{
	bPaused = true;
	StepDepth = -1;
	ResumedAt = &InNode;
	SetAt(InNode);
	Emit(TEXT("pause"), &InNode);
}

int32 FGameTestFlow::ListDepth() const
{
	int32 Depth = 0;
	for (int32 Index = 0; Index < Frames.Num(); ++Index)
	{
		// 附带 GM 包的那一层不算深度：主步骤和它前后的步骤在同一层，单步跳过它时照样停在下一步。
		const bool bHookLayer = Index > 0 && Frames[Index - 1].Kind == EFrameKind::Block && Frames[Index - 1].Node->bHookWrap;
		Depth += Frames[Index].Kind == EFrameKind::List && !bHookLayer ? 1 : 0;
	}
	return Depth;
}

int32 FGameTestFlow::CurrentIter() const
{
	for (int32 Index = Frames.Num() - 1; Index >= 0; --Index)
	{
		if (Frames[Index].Kind == EFrameKind::Block && Frames[Index].Iter >= 0)
		{
			return Frames[Index].Iter;
		}
	}
	return -1;
}

void FGameTestFlow::SetAt(const FGameTestNode& InNode)
{
	AtKey = InNode.Key();
	AtPath = InNode.Path;
}

//~ 记录、推送 ---------------------------------------------------------------

void FGameTestFlow::AddRecord(const FGameTestNode& InNode, EGameTestStepStatus InStatus, const FString& InMessage, double InStartedAt, double InNowSeconds, const TSharedPtr<FJsonObject>& InPayload, bool bInBlock)
{
	FGameTestStepRecord Record;
	Record.Step = InNode.Step;
	Record.Step.Id = InNode.Id;
	Record.Step.Path = InNode.Path;
	Record.Status = InStatus;
	Record.Message = InMessage;
	Record.Payload = WithStepVars(InNode, InStatus, InPayload);
	Record.StartedAt = InStartedAt;
	Record.FinishedAt = InNowSeconds;
	Record.bBlock = bInBlock;
	Record.Iter = CurrentIter();
	Record.bWarn = InNode.bHookNote && InStatus == EGameTestStepStatus::Failed;
	Report.Steps.Add(MoveTemp(Record));
}

TSharedPtr<FJsonObject> FGameTestFlow::WithStepVars(const FGameTestNode& InNode, EGameTestStepStatus InStatus, const TSharedPtr<FJsonObject>& InPayload) const
{
	// 没真跑的（跳过、禁用、随前一步已达成）变量值和这一步无关，不写。
	if (!Context.IsValid() || (InStatus != EGameTestStepStatus::Succeeded && InStatus != EGameTestStepStatus::Failed))
	{
		return InPayload;
	}
	TSet<FString> Names;
	GameTestStepVars::CollectNames(InNode, Names);
	const TSharedPtr<FJsonObject> Vars = GameTestStepVars::Pick(*Context, Names);
	if (!Vars.IsValid())
	{
		return InPayload;
	}
	// 复制一份再加：动作给的 payload 可能还被别处拿着（块的 Frame、单步结果），不改原对象。
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	if (InPayload.IsValid())
	{
		Out->Values = InPayload->Values;
	}
	Out->SetObjectField(TEXT("vars"), Vars);
	return Out;
}

void FGameTestFlow::Emit(const TCHAR* InPhase, const FGameTestNode* InNode, const TCHAR* InStatus, int32 InIter, const FString& InGuard, double InMs)
{
	if (!OnFlowEvent)
	{
		return;
	}
	TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
	Event->SetStringField(TEXT("phase"), InPhase);
	const FString Id = InNode ? InNode->Id : (FCString::Strcmp(InPhase, TEXT("pause")) == 0 ? AtKey : FString());
	const FString Path = InNode ? InNode->Path : (FCString::Strcmp(InPhase, TEXT("pause")) == 0 ? AtPath : FString());
	if (!Id.IsEmpty())
	{
		Event->SetStringField(TEXT("id"), Id);
	}
	if (!Path.IsEmpty())
	{
		Event->SetStringField(TEXT("path"), Path);
	}
	if (InStatus)
	{
		Event->SetStringField(TEXT("status"), InStatus);
	}
	if (InIter >= 0)
	{
		Event->SetNumberField(TEXT("iter"), InIter);
	}
	if (!InGuard.IsEmpty())
	{
		Event->SetStringField(TEXT("guard"), InGuard);
	}
	if (InMs >= 0.0)
	{
		Event->SetNumberField(TEXT("ms"), FMath::RoundToInt(InMs));
	}
	if (FCString::Strcmp(InPhase, TEXT("pause")) == 0)
	{
		Event->SetObjectField(TEXT("vars"), Context->SnapshotVars(4000));
	}
	OnFlowEvent(Event);
}

bool FGameTestFlow::CheckNoErrors(uint64 InErrorSeq, uint64 InLuaErrorSeq, FString& OutError) const
{
	const uint64 LogErrors = Context->LogTap ? Context->LogTap->GetErrorSeq() - InErrorSeq : 0;
	const uint64 LuaErrors = GameTestLuaBridge::ErrorSeq() - InLuaErrorSeq;
	if (LogErrors == 0 && LuaErrors == 0)
	{
		return true;
	}
	OutError = FString::Printf(TEXT("expectNoErrors：执行期间日志里出了 %llu 条错误、%llu 条 Lua 报错"), LogErrors, LuaErrors);
	if (LogErrors > 0 && Context->LogTap)
	{
		OutError += FString::Printf(TEXT("，最近一条：%s"), *Context->LogTap->GetLastError());
	}
	return false;
}
