// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestResolve.h"

#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestWorld.h"
#include "Observe/GameTestSelector.h"
#include "Observe/GameTestSnapshot.h"
#include "Observe/GameTestWidgetNode.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "Components/Widget.h"

namespace
{
	/** 找不到时候选清单的长度：够看出该怎么改，几十个选择器的结果放在一起也不超长。 */
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
		for (const TCHAR* Field : { TEXT("target"), TEXT("toTarget") })
		{
			FString Value;
			if (GameTestJson::TryGetString(*InSteps[Index], Field, Value) && !Value.TrimStartAndEnd().IsEmpty())
			{
				FTarget Target;
				Target.Target = Value.TrimStartAndEnd();
				Target.Step = Index;
				Target.Field = Field;
				Out.Add(MoveTemp(Target));
			}
		}
	}
	return Out;
}

bool GameTestResolve::Resolve(
	const TArray<FTarget>& InTargets,
	TMap<FString, TWeakObjectPtr<UWidget>>* InHandles,
	TSharedPtr<FJsonObject>& OutResult,
	FString& OutError)
{
	UWorld* World = GameTestWorld::FindGameWorld();
	if (World == nullptr)
	{
		OutError = TEXT("没有正在跑的游戏世界：选择器要对着游戏里的界面核对，先开 PIE 并走到界面出现的地方再核对");
		return false;
	}

	// 屏只采一次（只要结构）：几十个选择器各采一遍控件树太慢，而且这一刻的界面对所有选择器都一样
	TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
	GameTestSnapshot::CaptureScreens(World, Screens, /*bInLight=*/true);

	// 同一个选择器在场景里常出现好几次（点了又点）：只解析一次
	TMap<FString, TSharedPtr<FJsonObject>> Cache;
	TArray<TSharedPtr<FJsonValue>> Results;
	int32 Found = 0;
	const int32 Count = FMath::Min(InTargets.Num(), MaxTargets);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FTarget& Target = InTargets[Index];
		TSharedPtr<FJsonObject> Resolved = Cache.FindRef(Target.Target);
		if (!Resolved.IsValid())
		{
			Resolved = MakeShared<FJsonObject>();
			TSharedPtr<FGameTestWidgetNode> Node;
			FString Error;
			// @句柄 走共用的解析（句柄表在子系统里）；其余对着这次采的屏解析
			const bool bOk = Target.Target.StartsWith(TEXT("@"))
				? GameTestHandlers::TryResolveTarget(Target.Target, InHandles, Node, Error)
				: GameTestSelector::ResolveText(Screens, Target.Target, Node, Error);
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
		Item->SetStringField(TEXT("target"), Target.Target);
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
	OutResult->SetNumberField(TEXT("screens"), Screens.Num());
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
