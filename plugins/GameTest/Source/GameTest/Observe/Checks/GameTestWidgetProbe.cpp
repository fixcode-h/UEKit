// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Checks/GameTestWidgetProbe.h"

#include "Blueprint/GameViewportSubsystem.h"
#include "Blueprint/UserWidget.h"
#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestWorld.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Observe/GameTestSnapshot.h"
#include "Observe/GameTestWidgetNode.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/SWidget.h"

namespace GameTestWidgetProbe
{
	UWorld* WorldOf(const FGameTestFlowContext& InContext)
	{
		return InContext.World ? InContext.World : GameTestWorld::FindGameWorld();
	}

	FGameTestWidgetLook Look(const FString& InTarget, FGameTestFlowContext& InContext)
	{
		FGameTestWidgetLook Look;
		if (!GameTestRefs::EvalSelector(InTarget, InContext, Look.Target, Look.Error))
		{
			Look.Error = FString::Printf(TEXT("选择器里的变量取不到：%s"), *Look.Error);
			return Look;
		}
		Look.bResolved = GameTestHandlers::TryResolveTarget(Look.Target, InContext.Handles, Look.Node, Look.Error) && Look.Node.IsValid();
		return Look;
	}

	bool IsOnScreen(const FGameTestWidgetNode& InNode)
	{
		TSharedPtr<SWidget> Slate = InNode.Slate.Pin();
		if (!Slate.IsValid() && InNode.Widget.IsValid())
		{
			Slate = InNode.Widget->GetCachedWidget();
		}
		if (!Slate.IsValid())
		{
			return false;
		}
		for (TSharedPtr<SWidget> Current = Slate; Current.IsValid(); Current = Current->GetParentWidget())
		{
			if (!Current->GetVisibility().IsVisible())
			{
				return false;
			}
		}
		const FVector2D Size = FVector2D(Slate->GetCachedGeometry().GetLocalSize());
		return Size.X >= 1.0 && Size.Y >= 1.0;
	}

	bool ClassMatches(const UClass* InClass, const FString& InName)
	{
		if (InName.IsEmpty())
		{
			return false;
		}
		// 名字可以写 WBP_X、WBP_X_C、UMyBase、MyBase 或资产路径；按继承链往上找，子类也算（用户 2026-11-18：界面按继承认）。
		FString Wanted = InName;
		Wanted.RemoveFromEnd(TEXT("_C"));
		const bool bPath = Wanted.StartsWith(TEXT("/"));
		for (const UClass* Class = InClass; Class; Class = Class->GetSuperClass())
		{
			if (bPath)
			{
				FString ClassPath = Class->GetPathName();
				ClassPath.RemoveFromEnd(TEXT("_C"));
				// 蓝图类路径是 /Game/UI/WBP_X.WBP_X_C，资产路径是 /Game/UI/WBP_X。
				if (ClassPath.Equals(Wanted, ESearchCase::IgnoreCase) || ClassPath.StartsWith(Wanted + TEXT("."), ESearchCase::IgnoreCase))
				{
					return true;
				}
				continue;
			}
			const FString Friendly = GameTestSnapshot::FriendlyClassName(Class);
			if (Friendly.Equals(Wanted, ESearchCase::IgnoreCase) || Class->GetName().Equals(InName, ESearchCase::IgnoreCase)
				|| (Class->GetPrefixCPP() + Class->GetName()).Equals(InName, ESearchCase::IgnoreCase))
			{
				return true;
			}
			if (Class == UUserWidget::StaticClass())
			{
				break;
			}
		}
		return false;
	}

	bool IsScreenShown(const FString& InScreen, UWorld* InWorld, int32* OutCount)
	{
		int32 Count = 0;
		bool bShown = false;
		if (InWorld)
		{
			for (TObjectIterator<UUserWidget> It; It; ++It)
			{
				UUserWidget* Widget = *It;
				if (!IsValid(Widget) || Widget->IsTemplate() || Widget->GetWorld() != InWorld || !Widget->GetCachedWidget().IsValid())
				{
					continue;
				}
				if (!ClassMatches(Widget->GetClass(), InScreen))
				{
					continue;
				}
				++Count;
				FGameTestWidgetNode Node;
				Node.Widget = Widget;
				bShown = bShown || IsOnScreen(Node);
			}
		}
		if (OutCount)
		{
			*OutCount = Count;
		}
		return bShown;
	}

	FString ScreenOf(const FString& InTarget)
	{
		if (InTarget.StartsWith(TEXT("@")) || InTarget.StartsWith(TEXT(">")))
		{
			return FString();
		}
		int32 Colon = INDEX_NONE;
		if (!InTarget.FindChar(TEXT(':'), Colon) || Colon <= 0)
		{
			return FString();
		}
		const FString Screen = InTarget.Left(Colon);
		return Screen == TEXT("*") || Screen.Contains(TEXT(">")) ? FString() : Screen;
	}

	void ListScreens(UWorld* InWorld, TArray<UUserWidget*>& OutScreens)
	{
		OutScreens.Reset();
		if (!InWorld)
		{
			return;
		}
		TArray<TPair<int32, UUserWidget*>> Ordered;
		// 它是引擎子系统：直接从 GEngine 取，避开 Get(World)（5.8 弃用）和无参 Get 在各版本的差异
		UGameViewportSubsystem* Viewport = GEngine ? GEngine->GetEngineSubsystem<UGameViewportSubsystem>() : nullptr;
		for (TObjectIterator<UUserWidget> It; It; ++It)
		{
			UUserWidget* Widget = *It;
			if (!IsValid(Widget) || Widget->IsTemplate() || Widget->GetWorld() != InWorld || !Widget->IsInViewport())
			{
				continue;
			}
			const int32 ZOrder = Viewport ? Viewport->GetWidgetSlot(Widget).ZOrder : 0;
			Ordered.Add(TPair<int32, UUserWidget*>(ZOrder, Widget));
		}
		// ZOrder 大的在上面；一样时后建的在上面（对象编号大的后建）。
		Ordered.Sort([](const TPair<int32, UUserWidget*>& A, const TPair<int32, UUserWidget*>& B)
		{
			return A.Key != B.Key ? A.Key > B.Key : A.Value->GetUniqueID() > B.Value->GetUniqueID();
		});
		for (const TPair<int32, UUserWidget*>& Pair : Ordered)
		{
			OutScreens.Add(Pair.Value);
		}
	}

	bool NumberInText(const FString& InText, double& OutNumber)
	{
		int32 At = 0;
		while (At < InText.Len() && !FChar::IsDigit(InText[At]))
		{
			++At;
		}
		if (At >= InText.Len())
		{
			return false;
		}
		// 数字前紧挨着的负号算进去（「-5」），「HP-5」这种被字母隔开的不算。
		const bool bNegative = At > 0 && (InText[At - 1] == TEXT('-') || InText[At - 1] == TEXT('−')) && (At == 1 || !FChar::IsAlnum(InText[At - 2]));
		FString Digits;
		bool bDot = false;
		for (; At < InText.Len(); ++At)
		{
			const TCHAR C = InText[At];
			if (FChar::IsDigit(C))
			{
				Digits.AppendChar(C);
			}
			else if (C == TEXT(',') && At + 1 < InText.Len() && FChar::IsDigit(InText[At + 1]) && !bDot)
			{
				continue;
			}
			else if (C == TEXT('.') && !bDot && At + 1 < InText.Len() && FChar::IsDigit(InText[At + 1]))
			{
				bDot = true;
				Digits.AppendChar(C);
			}
			else
			{
				break;
			}
		}
		double Number = FCString::Atod(*Digits);
		// 单位后面还连着字母（「5min」「3wins」）就不是单位。
		const bool bUnitAlone = At + 1 >= InText.Len() || !(InText[At + 1] < 0x80 && FChar::IsAlpha(InText[At + 1]));
		if (At < InText.Len() && bUnitAlone)
		{
			const TCHAR Unit = InText[At];
			Number *= Unit == TEXT('万') || Unit == TEXT('w') || Unit == TEXT('W') ? 1e4
				: Unit == TEXT('亿') ? 1e8
				: Unit == TEXT('k') || Unit == TEXT('K') ? 1e3
				: Unit == TEXT('m') || Unit == TEXT('M') ? 1e6
				: 1.0;
		}
		OutNumber = bNegative ? -Number : Number;
		return true;
	}

	int32 CountOf(const FGameTestWidgetNode& InNode)
	{
		return InNode.Items.Num() > 0 ? InNode.Items.Num() : InNode.Children.Num();
	}
}
