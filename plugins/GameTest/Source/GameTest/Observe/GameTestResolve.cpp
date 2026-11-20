// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestResolve.h"

#include "Core/GameTestHandlerShared.h"
#include "GameTestCompat.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestWorld.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Observe/Ref/GameTestRefAmbient.h"
#include "Observe/GameTestWidgetNode.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "Components/Widget.h"

namespace
{
	/** 找不到时候选清单的长度：够看出该怎么改，几十个目标的结果放在一起也不超长。 */
	constexpr int32 ResolveMaxErrorChars = 600;
}

TArray<GameTestResolve::FTarget> GameTestResolve::CollectTargets(const TArray<TSharedPtr<FJsonObject>>& InSteps)
{
	TArray<FTarget> Out;
	for (int32 Index = 0; Index < InSteps.Num(); ++Index)
	{
		if (!InSteps[Index].IsValid())
		{
			continue;
		}
		for (const TCHAR* Field : { TEXT("target"), TEXT("toTarget"), TEXT("widget") })
		{
			if (!InSteps[Index]->HasField(Field))
			{
				continue;
			}
			FTarget Target;
			if (!GameTestHandlers::TargetText(*InSteps[Index], Field, Target.Target, Target.Error) || !Target.Target.IsEmpty())
			{
				Target.Step = Index;
				Target.Field = Field;
				Out.Add(MoveTemp(Target));
			}
		}
	}
	return Out;
}

GameTestResolve::FTarget GameTestResolve::FromValue(const TSharedPtr<FJsonValue>& InValue)
{
	FTarget Target;
	const TSharedRef<FJsonObject> Holder = MakeShared<FJsonObject>();
	Holder->SetField(TEXT("target"), InValue.IsValid() ? InValue : MakeShared<FJsonValueNull>());
	if (GameTestHandlers::TargetText(*Holder, TEXT("target"), Target.Target, Target.Error) && Target.Target.IsEmpty())
	{
		Target.Error = TEXT("targets 里有空项");
	}
	return Target;
}

bool GameTestResolve::Resolve(const TArray<FTarget>& InTargets, TMap<FString, TWeakObjectPtr<UWidget>>* InHandles, const FJsonObject* InObjects, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
{
	UWorld* World = GameTestWorld::FindGameWorld();
	if (World == nullptr)
	{
		OutError = TEXT("没有正在跑的游戏世界：目标要对着游戏里的界面核对，先开 PIE 并走到界面出现的地方再核对");
		return false;
	}

	// 解析用的上下文：场景的 objects 放进去，命名引用才认得；只读
	FGameTestFlowContext Flow;
	Flow.World = World;
	Flow.Handles = InHandles;
	if (InObjects != nullptr)
	{
		for (const auto& Pair : InObjects->Values)
		{
			const TSharedPtr<FJsonObject>* Def = nullptr;
			if (Pair.Value.IsValid() && Pair.Value->TryGetObject(Def) && Def != nullptr && Def->IsValid())
			{
				Flow.GetScope().ObjectDefs.Add(GAMETEST_JSON_KEY(Pair.Key), *Def);
			}
		}
	}
	FGameTestFlowContext::FReadOnlyScope ReadOnly(Flow);
	GameTestRefAmbient::FScope Ambient(&Flow);

	// 同一个目标在场景里常出现好几次（点了又点）：只解析一次
	TMap<FString, TSharedPtr<FJsonObject>> Cache;
	TArray<TSharedPtr<FJsonValue>> Results;
	int32 Found = 0;
	const int32 Count = FMath::Min(InTargets.Num(), MaxTargets);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FTarget& Target = InTargets[Index];
		TSharedPtr<FJsonObject> Resolved = Target.Error.IsEmpty() ? Cache.FindRef(Target.Target) : nullptr;
		if (!Target.Error.IsEmpty())
		{
			Resolved = MakeShared<FJsonObject>();
			Resolved->SetBoolField(TEXT("found"), false);
			Resolved->SetStringField(TEXT("error"), Target.Error.Left(ResolveMaxErrorChars));
		}
		else if (!Resolved.IsValid())
		{
			Resolved = MakeShared<FJsonObject>();
			TSharedPtr<FGameTestWidgetNode> Node;
			FString Error;
			// 引用和 @句柄 都走动作用的同一个解析（GameTestHandlers::TryResolveTarget）
			const bool bOk = GameTestHandlers::TryResolveTarget(Target.Target, InHandles, Node, Error);
			Resolved->SetBoolField(TEXT("found"), bOk && Node.IsValid());
			if (bOk && Node.IsValid())
			{
				Resolved->SetStringField(TEXT("path"), Node->Path);
				Resolved->SetStringField(TEXT("class"), Node->ClassName);
				if (!Node->Text.IsEmpty())
				{
					Resolved->SetStringField(TEXT("text"), Node->Text.Left(80));
				}
				Resolved->SetBoolField(TEXT("visible"), GameTestHandlers::IsActuallyVisible(*Node));
				Resolved->SetBoolField(TEXT("enabled"), Node->bEnabled);
			}
			else
			{
				Resolved->SetStringField(TEXT("error"), Error.Left(ResolveMaxErrorChars));
			}
			Cache.Add(Target.Target, Resolved);
		}

		const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>(*Resolved);
		Item->SetStringField(TEXT("target"), Target.Target.IsEmpty() ? FString() : GameTestHandlers::DescribeTarget(Target.Target));
		if (Target.Step >= 0)
		{
			Item->SetNumberField(TEXT("step"), Target.Step);
			Item->SetStringField(TEXT("field"), Target.Field);
		}
		bool bFound = false;
		Item->TryGetBoolField(TEXT("found"), bFound);
		Found += bFound ? 1 : 0;
		Results.Add(MakeShared<FJsonValueObject>(Item));
	}

	OutResult = MakeShared<FJsonObject>();
	OutResult->SetStringField(TEXT("map"), World->GetOutermost()->GetName());
	OutResult->SetNumberField(TEXT("checked"), Count);
	OutResult->SetNumberField(TEXT("found"), Found);
	OutResult->SetNumberField(TEXT("missing"), Count - Found);
	if (InTargets.Num() > Count)
	{
		OutResult->SetNumberField(TEXT("omitted"), InTargets.Num() - Count);
	}
	OutResult->SetArrayField(TEXT("results"), Results);
	OutResult->SetStringField(TEXT("hint"), TEXT("只看这一刻屏上的界面：后面步骤才打开的界面现在找不到是正常的，走到那一步再核对"));
	return true;
}
