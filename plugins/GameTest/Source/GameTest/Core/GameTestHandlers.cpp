// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestHandlerShared.h"

#include "Core/GameTestJson.h"
#include "Core/GameTestWorld.h"
#include "Observe/GameTestSelector.h"
#include "Observe/GameTestSnapshot.h"
#include "Observe/GameTestWidgetNode.h"

#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"

namespace GameTestHandlers
{
	/**
	 * 选择器 / `@句柄` → 控件节点。
	 *
	 * 放在这里（而不是某个子域的匿名 namespace）是因为观察与交互两个子域都要用：
	 * 各写一份必然漂移。
	 */
	bool TryResolveTarget(
		const FString& InTarget,
		TMap<FString, TWeakObjectPtr<UWidget>>* InHandles,
		TSharedPtr<FGameTestWidgetNode>& OutNode,
		FString& OutMessage)
	{
		if (InTarget.IsEmpty())
		{
			OutMessage = TEXT("没有给 target");
			return false;
		}

		if (InTarget.StartsWith(TEXT("@")))
		{
			const FString Handle = InTarget.RightChop(1);
			if (InHandles == nullptr)
			{
				OutMessage = TEXT("句柄表不可用");
				return false;
			}

			const TWeakObjectPtr<UWidget>* Found = InHandles->Find(Handle);
			if (Found == nullptr)
			{
				OutMessage = FString::Printf(TEXT("没有名为 '%s' 的句柄（先用 find 捕获它）"), *Handle);
				return false;
			}

			UWidget* Widget = Found->Get();
			if (Widget == nullptr)
			{
				OutMessage = FString::Printf(TEXT("句柄 '%s' 已失效（控件被销毁或世界重载过），重新 find 一次"), *Handle);
				return false;
			}

			// 句柄每次重新采一遍子树：拿到的永远是此刻的状态与条目，而不是捕获那一刻的快照。
			OutNode = GameTestSnapshot::CaptureSubtree(Widget, FString(), /*InIsScreenRoot=*/false);
			return OutNode.IsValid();
		}

		UWorld* World = GameTestWorld::FindGameWorld();
		if (World == nullptr)
		{
			OutMessage = TEXT("当前没有正在跑的游戏世界（先 pie.start，或 wait until=pieReady）");
			return false;
		}

		// 只要结构不要属性：回放的自动等待每帧都会走到这里，属性导出是最贵的一块。
		TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
		GameTestSnapshot::CaptureScreens(World, Screens, /*bInLight=*/true);
		if (Screens.Num() == 0 && !InTarget.StartsWith(TEXT(">>")))
		{
			OutMessage = TEXT("世界里没有挂在视口上的 UMG（用 widget.add 造一个靶子，或确认界面真的被打开了）");
			return false;
		}

		return GameTestSelector::ResolveText(Screens, InTarget, OutNode, OutMessage);
	}

	bool TryResolveStepTarget(FGameTestStepContext& InContext, TSharedPtr<FGameTestWidgetNode>& OutNode, FString& OutError)
	{
		if (!TryResolveTarget(InContext.Step->Target, InContext.Handles, OutNode, OutError))
		{
			return false;
		}

		InContext.TouchedWidget = OutNode->Widget;

		// 矩形照抄快照的坐标（截图标注用的同一份数）：换算留给读的人，写入的人不做第二套。
		InContext.bTouchedHasRect = OutNode->bHasGeometry && OutNode->Size.X >= 1.0 && OutNode->Size.Y >= 1.0;
		InContext.TouchedMin = OutNode->Position;
		InContext.TouchedMax = OutNode->Position + OutNode->Size;
		return true;
	}

	bool IsActuallyVisible(const FGameTestWidgetNode& InNode)
	{
		return InNode.Visibility == TEXT("Visible")
			|| InNode.Visibility == TEXT("HitTestInvisible")
			|| InNode.Visibility == TEXT("SelfHitTestInvisible");
	}

	bool TryReadClientField(const FJsonObject& InParams, FString& OutClientText, bool& bOutSpecified, FString& OutError)
	{
		bOutSpecified = false;
		OutClientText.Reset();

		if (!InParams.Values.Contains(TEXT("client")))
		{
			return true;
		}

		bOutSpecified = true;

		if (GameTestJson::TryGetString(InParams, TEXT("client"), OutClientText))
		{
			return true;
		}

		double Number = 0.0;
		if (GameTestJson::TryGetNumber(InParams, TEXT("client"), Number))
		{
			OutClientText = FString::FromInt(FMath::RoundToInt(Number));
			return true;
		}

		OutError = TEXT("client 字段只能是 \"server\" 或从 0 起的序号");
		return false;
	}

	UWorld* ResolveStepWorld(const FGameTestStepContext& InContext, FString& OutError)
	{
		FString ClientText;
		bool bSpecified = false;
		if (!TryReadClientField(*InContext.Step->Params, ClientText, bSpecified, OutError))
		{
			return nullptr;
		}

		UWorld* World = GameTestWorld::ResolveWorld(ClientText, bSpecified, OutError);
		if (World == nullptr && OutError.IsEmpty())
		{
			OutError = TEXT("当前没有正在跑的游戏世界（先 pie.start，或 wait until=pieReady）");
		}
		return World;
	}
}
