// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Flow/Deep/GameTestDeepCheck.h"

#include "Core/Flow/GameTestNode.h"
#include "Core/Script/GameTestBuiltins.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace GameTestDeepWalk
{
	void VisitValue(const TSharedPtr<FJsonValue>& InValue, FGameTestDeepEnv& InEnv, const TSet<FString>& InSkipKeys);

	/** 一个 JSON 对象：先交给每个检查器，再往里走（子步骤列表由节点遍历负责，这里跳过，免得查两遍）。 */
	void VisitObject(const FJsonObject& InJson, FGameTestDeepEnv& InEnv, const TSet<FString>& InSkipKeys)
	{
		for (const TSharedRef<IGameTestDeepCheck>& Check : GameTestDeepChecks::Get().GetAll())
		{
			Check->CheckObject(InJson, InEnv);
		}
		static const TSet<FString> NoSkip;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : InJson.Values)
		{
			if (!InSkipKeys.Contains(Pair.Key))
			{
				VisitValue(Pair.Value, InEnv, NoSkip);
			}
		}
	}

	void VisitValue(const TSharedPtr<FJsonValue>& InValue, FGameTestDeepEnv& InEnv, const TSet<FString>& InSkipKeys)
	{
		if (!InValue.IsValid())
		{
			return;
		}
		if (InValue->Type == EJson::Object)
		{
			if (const TSharedPtr<FJsonObject> Object = InValue->AsObject())
			{
				VisitObject(*Object, InEnv, InSkipKeys);
			}
		}
		else if (InValue->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Item : InValue->AsArray())
			{
				VisitValue(Item, InEnv, InSkipKeys);
			}
		}
	}

	void VisitList(const FGameTestNodeList& InList, FGameTestDeepEnv& InEnv);

	void VisitNode(const FGameTestNode& InNode, FGameTestDeepEnv& InEnv)
	{
		if (InNode.bDisabled)
		{
			return;
		}
		{
			// 错误挂在这个节点的路径（已是全路径）和 id 上，面板能定位到流程图里的那个节点；
			// 作用域只包住本节点，子节点在外面走，路径不会叠两遍
			FGameTestDiagSink::FScope Scope(InEnv.Sink, InNode.Path, InNode.Id);
			TSet<FString> ChildKeys;
			for (const FGameTestNodeList& List : InNode.Lists)
			{
				ChildKeys.Add(List.Key);
			}
			if (InNode.Json.IsValid())
			{
				VisitObject(*InNode.Json, InEnv, ChildKeys);
			}
		}
		for (const FGameTestNodeList& List : InNode.Lists)
		{
			VisitList(List, InEnv);
		}
	}

	void VisitList(const FGameTestNodeList& InList, FGameTestDeepEnv& InEnv)
	{
		for (const TSharedRef<FGameTestNode>& Node : InList.Nodes)
		{
			VisitNode(*Node, InEnv);
		}
	}
}

TGameTestRegistry<IGameTestDeepCheck>& GameTestDeepChecks::Get()
{
	static TGameTestRegistry<IGameTestDeepCheck> Registry;
	GameTestBuiltins::EnsureRegistered();
	return Registry;
}

void GameTestDeepChecks::Run(const FGameTestScript& InScript, FGameTestFlowContext& InContext, FGameTestDiagSink& InSink, FGameTestDeepStats& OutStats)
{
	FGameTestDeepEnv Env{ InContext, InSink, OutStats, InScript, InContext.World != nullptr };
	for (const TSharedRef<IGameTestDeepCheck>& Check : Get().GetAll())
	{
		Check->CheckScript(InScript, Env);
	}
	GameTestDeepWalk::VisitList(InScript.Setup, Env);
	GameTestDeepWalk::VisitList(InScript.Steps, Env);
	GameTestDeepWalk::VisitList(InScript.Teardown, Env);
}

TSharedRef<FJsonObject> GameTestDeepChecks::StatsToJson(const FGameTestDeepStats& InStats)
{
	const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("functions"), InStats.Functions);
	Out->SetNumberField(TEXT("refs"), InStats.Refs);
	Out->SetNumberField(TEXT("paths"), InStats.Paths);
	Out->SetNumberField(TEXT("apis"), InStats.Apis);
	Out->SetNumberField(TEXT("objects"), InStats.Objects);
	return Out;
}
