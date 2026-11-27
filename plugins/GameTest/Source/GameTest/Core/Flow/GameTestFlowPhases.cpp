// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Flow/GameTestFlow.h"
#include "Core/Flow/GameTestNode.h"
#include "Dom/JsonObject.h"

/**
 * 三段（执行前 setup / 执行 steps / 执行后 teardown，2.8.0）的进度和调用层级：给 gt.job、推送事件和记录用。
 * 和主循环分开放：这里只读帧和记录，不改流程状态。
 */

TArray<FString> FGameTestFlow::CurrentCalls() const
{
	TArray<FString> Calls;
	for (const FFrame& Frame : Frames)
	{
		if (Frame.Kind == EFrameKind::Block && Frame.Node != nullptr && Frame.Node->Action == FName(TEXT("call")))
		{
			Calls.Add(Frame.Node->Key());
		}
	}
	return Calls;
}

const TCHAR* FGameTestFlow::GetPhase() const
{
	if (bInTeardown)
	{
		return TEXT("teardown");
	}
	return bSetupDone ? TEXT("steps") : TEXT("setup");
}

TSharedPtr<FJsonObject> FGameTestFlow::PhaseCounts() const
{
	// 顶层节点跑完一个（不管通过、失败、跳过、禁用）算一步：子脚本里的（有 calls）、块里的（bNested）不数，同一个节点只数一次
	TSet<FString> Setup;
	TSet<FString> Steps;
	TSet<FString> Teardown;
	for (const FGameTestStepRecord& Record : Report.Steps)
	{
		if (Record.Calls.Num() > 0 || Record.Step.bNested || Record.Step.Path.IsEmpty())
		{
			continue;
		}
		TSet<FString>& Into = Record.Step.bTeardown ? Teardown : Record.Step.bSetup ? Setup : Steps;
		Into.Add(Record.Step.Path);
	}
	const TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	const auto Add = [&Out](const TCHAR* InName, int32 InDone, int32 InTotal)
	{
		const TSharedPtr<FJsonObject> Phase = MakeShared<FJsonObject>();
		Phase->SetNumberField(TEXT("done"), FMath::Min(InDone, InTotal));
		Phase->SetNumberField(TEXT("total"), InTotal);
		Out->SetObjectField(InName, Phase);
	};
	Add(TEXT("setup"), Setup.Num(), Script->Setup.Nodes.Num());
	Add(TEXT("steps"), Steps.Num(), Script->Steps.Nodes.Num());
	Add(TEXT("teardown"), Teardown.Num(), Script->Teardown.Nodes.Num());
	return Out;
}
