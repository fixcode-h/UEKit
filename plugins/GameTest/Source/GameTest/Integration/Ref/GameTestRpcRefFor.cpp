// Copyright Epic Games, Inc. All Rights Reserved.

// gt.refFor（docs/game-test-ref.md 7.4）：从游戏里点到的东西出引用；gt.resolve 的 refs（7.6）：引用此刻命中几个、各是谁。
// 以及 Integration/Ref 几个操作共用的小工具（临时上下文、node / target 参数）。

#include "Integration/Ref/GameTestRpcRef.h"

#include "GameTestCompat.h"
#include "Core/GameTestWorld.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTestSubsystem.h"
#include "Observe/GameTestInspect.h"
#include "Observe/Ref/GameTestPropPath.h"
#include "Observe/Ref/GameTestRef.h"
#include "Observe/Ref/GameTestRefFor.h"
#include "Observe/Ref/GameTestRefResolve.h"

#include "Components/Widget.h"

namespace GameTestRpcRefForImpl
{
	constexpr int32 MaxRefs = 50;
	constexpr int32 MaxCandidates = 8;
}

void GameTestRpcRef::MakeContext(FGameTestFlowContext& OutContext)
{
	OutContext.World = GameTestWorld::FindGameWorld();
	if (UGameTestSubsystem* Subsystem = UGameTestSubsystem::Get())
	{
		OutContext.Handles = Subsystem->GetHandles();
	}
}

UObject* GameTestRpcRef::ObjectFromArgs(const FJsonObject& InArgs, FGameTestFlowContext& InContext, FString& OutError)
{
	FString NodeId;
	if (InArgs.TryGetStringField(TEXT("node"), NodeId) && !NodeId.IsEmpty())
	{
		UWidget* Widget = FindNode(NodeId);
		if (Widget == nullptr)
		{
			OutError = FString::Printf(TEXT("没有节点 %s（gt.tree 每次调用重新编号，控件没了 id 也失效）"), *NodeId);
		}
		return Widget;
	}
	if (InArgs.HasField(TEXT("target")))
	{
		const FGameTestTargetResult Result = GameTestRefResolve::ResolveTarget(InArgs.TryGetField(TEXT("target")), InContext);
		if (!Result.IsOk())
		{
			OutError = FString::Printf(TEXT("%s：%s"), *Result.Code.ToString(), *Result.Error);
			return nullptr;
		}
		return Result.Get();
	}
	OutError = TEXT("要 node（gt.tree 的 id）或 target（部件写法）");
	return nullptr;
}

TSharedRef<FJsonObject> GameTestRpcRef::RefJsonFor(UWidget* InWidget, FGameTestFlowContext& InContext, int32 InLevel)
{
	return GameTestRefFor::ToJson(InWidget, InContext, InLevel, [](const UWidget* InNode) { return NodeIdOf(InNode); });
}

bool GameTestRpcRef::RefFor(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
{
	FGameTestFlowContext Flow;
	MakeContext(Flow);
	if (Flow.World == nullptr || !InArgs.IsValid())
	{
		OutError = TEXT("游戏没在跑：gt.refFor 要开着游戏");
		return false;
	}
	int32 Level = 0;
	InArgs->TryGetNumberField(TEXT("level"), Level);
	UWidget* Widget = nullptr;
	FString NodeId;
	if (InArgs->TryGetStringField(TEXT("node"), NodeId) && !NodeId.IsEmpty())
	{
		Widget = FindNode(NodeId);
		if (Widget == nullptr)
		{
			OutError = FString::Printf(TEXT("没有节点 %s（gt.tree 每次调用重新编号）"), *NodeId);
			return false;
		}
	}
	else if (InArgs->HasField(TEXT("x")) && InArgs->HasField(TEXT("y")))
	{
		// 按点：和 gt.pick at 同一条路（命中测试 + 偏向可交互控件），返回里已经有 chain / ref / target
		const FVector2D Point(InArgs->GetNumberField(TEXT("x")), InArgs->GetNumberField(TEXT("y")));
		return GameTestInspect::Inspect(TOptional<FVector2D>(Point), OutResult, OutError, /*bInPreferInteractive=*/true);
	}
	else
	{
		OutError = TEXT("gt.refFor 要 node（gt.tree 的 id）或 x / y（视口归一化坐标）");
		return false;
	}
	OutResult = RefJsonFor(Widget, Flow, Level);
	return true;
}

bool GameTestRpcRef::HasRefs(const TSharedPtr<FJsonObject>& InArgs)
{
	return InArgs.IsValid() && InArgs->HasField(TEXT("refs"));
}

bool GameTestRpcRef::ResolveRefs(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
{
	using namespace GameTestRpcRefForImpl;
	const TArray<TSharedPtr<FJsonValue>>* Refs = nullptr;
	if (!InArgs->TryGetArrayField(TEXT("refs"), Refs) || Refs == nullptr || Refs->Num() == 0 || Refs->Num() > MaxRefs)
	{
		OutError = FString::Printf(TEXT("refs 要 1–%d 项：[{id?, ref, show?: [属性路径…]}]"), MaxRefs);
		return false;
	}
	FGameTestFlowContext Flow;
	MakeContext(Flow);
	// 场景的 objects 一起给时，ref 可以写名字
	const TSharedPtr<FJsonObject>* Objects = nullptr;
	if (InArgs->TryGetObjectField(TEXT("objects"), Objects) && Objects != nullptr && Objects->IsValid())
	{
		Flow.GetScope().AddObjects(**Objects);
	}
	FGameTestFlowContext::FReadOnlyScope ReadOnly(Flow);
	TArray<TSharedPtr<FJsonValue>> Items;
	for (int32 Index = 0; Index < Refs->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject>* Entry = nullptr;
		const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		if (!(*Refs)[Index].IsValid() || !(*Refs)[Index]->TryGetObject(Entry) || Entry == nullptr || !Entry->IsValid())
		{
			Item->SetStringField(TEXT("error"), TEXT("这一项要写 {id?, ref, show?}"));
			Items.Add(MakeShared<FJsonValueObject>(Item));
			continue;
		}
		FString Id = FString::FromInt(Index);
		(*Entry)->TryGetStringField(TEXT("id"), Id);
		Item->SetStringField(TEXT("id"), Id);
		FGameTestRef Ref;
		UObject* Direct = nullptr;
		FString Error;
		if (!GameTestRefResolve::ToRef((*Entry)->TryGetField(TEXT("ref")), Flow, Ref, Direct, Error))
		{
			Item->SetStringField(TEXT("error"), Error);
			Items.Add(MakeShared<FJsonValueObject>(Item));
			continue;
		}
		// show：每个候选额外要看的属性（面板的同类实例表「排除：因为哪条」）
		TArray<FGameTestIdentItem> Show = Ref.Ident;
		const TArray<TSharedPtr<FJsonValue>>* ShowProps = nullptr;
		if ((*Entry)->TryGetArrayField(TEXT("show"), ShowProps) && ShowProps != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *ShowProps)
			{
				FString Prop;
				if (Value.IsValid() && Value->TryGetString(Prop) && !Prop.IsEmpty() && !Show.ContainsByPredicate([&Prop](const FGameTestIdentItem& It) { return It.Prop == Prop; }))
				{
					FGameTestIdentItem Extra;
					Extra.Prop = Prop;
					Show.Add(Extra);
				}
			}
		}
		TArray<FGameTestRefCandidate> Candidates;
		FName Code;
		if (!GameTestRefResolve::Collect(Ref, Flow, Candidates, Error, Code))
		{
			Item->SetStringField(TEXT("error"), Error);
			Item->SetStringField(TEXT("code"), Code.ToString());
			Items.Add(MakeShared<FJsonValueObject>(Item));
			continue;
		}
		int32 Count = 0;
		TArray<TSharedPtr<FJsonValue>> CandidateJson;
		for (const FGameTestRefCandidate& Candidate : Candidates)
		{
			const bool bMatch = Ref.Ident.Num() == 0 || GameTestRefResolve::MatchesIdent(Candidate, Ref.Ident, Flow);
			Count += bMatch ? 1 : 0;
			if (CandidateJson.Num() < MaxCandidates)
			{
				const TSharedPtr<FJsonObject> One = GameTestRefResolve::CandidateToJson(Candidate, Show, Flow);
				One->SetBoolField(TEXT("match"), bMatch);
				// 面板把候选对上控件树：name 是控件名（同 gt.tree 的 name），调过 gt.tree 时再给节点 id
				const FString NodeId = NodeIdOf(Cast<UWidget>(Candidate.Object.Get()));
				if (!NodeId.IsEmpty())
				{
					One->SetStringField(TEXT("node"), NodeId);
				}
				CandidateJson.Add(MakeShared<FJsonValueObject>(One));
			}
		}
		Item->SetNumberField(TEXT("count"), Count);
		Item->SetNumberField(TEXT("total"), Candidates.Num());
		Item->SetArrayField(TEXT("candidates"), CandidateJson);
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}
	OutResult = MakeShared<FJsonObject>();
	OutResult->SetArrayField(TEXT("refs"), Items);
	return true;
}
