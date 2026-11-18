// Copyright Epic Games, Inc. All Rights Reserved.

#include "Operate/GameTestAutoScroll.h"

#include "Core/GameTestHandlerShared.h"
#include "GameTest.h"
#include "Observe/GameTestSelector.h"
#include "Observe/GameTestSlate.h"
#include "Observe/GameTestWidgetNode.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/ListView.h"
#include "Components/TileView.h"
#include "Components/ScrollBox.h"
#include "Components/Widget.h"
#include "Engine/World.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/SWidget.h"

namespace GameTestAutoScrollDetail
{
	/** 一步里最多滚几次滚动框：嵌套的滚动框一层一次，再多说明滚了也看不见（被裁掉、尺寸算错），别一直拖。 */
	constexpr int32 MaxScrollBoxAttempts = 4;

	/** 找不到多久才开始翻：界面刚打开时数据还没到、条目还没建出来，马上翻是白翻。 */
	constexpr double ScanDelaySeconds = 0.6;
	/** 翻一页后等多久再翻：虚拟化列表要到下一次布局才生成新条目，给它几帧，也让调用方找一次。 */
	constexpr double ScanIntervalSeconds = 0.2;
	/** 一个容器最多翻几页：防止长度一直在涨的列表（无限滚动）翻个没完。 */
	constexpr int32 MaxScanSteps = 300;
	/** 最多翻几个容器：屏上列表太多时翻不完，先翻选择器写的那个屏里的。 */
	constexpr int32 MaxScanBoxes = 12;

	void AddNote(GameTestAutoScroll::FState& InOutState, const FString& InNote)
	{
		InOutState.Note = InOutState.Note.IsEmpty() ? InNote : InOutState.Note + TEXT("；") + InNote;
	}

	/** 目标的矩形是不是整个落在滚动框的矩形里（绝对坐标，留 1 像素误差）。 */
	bool IsInside(const SWidget& InTarget, const SWidget& InBox)
	{
		const FGeometry& Target = InTarget.GetCachedGeometry();
		const FGeometry& Box = InBox.GetCachedGeometry();
		const FVector2D TargetMin = FVector2D(Target.GetAbsolutePosition());
		const FVector2D TargetMax = TargetMin + FVector2D(Target.GetAbsoluteSize());
		const FVector2D BoxMin = FVector2D(Box.GetAbsolutePosition());
		const FVector2D BoxMax = BoxMin + FVector2D(Box.GetAbsoluteSize());
		return TargetMin.X >= BoxMin.X - 1.0 && TargetMin.Y >= BoxMin.Y - 1.0
			&& TargetMax.X <= BoxMax.X + 1.0 && TargetMax.Y <= BoxMax.Y + 1.0;
	}
}

bool GameTestAutoScroll::ScrollToUngeneratedEntry(FState& InOutState)
{
	using namespace GameTestAutoScrollDetail;
	TWeakObjectPtr<UListView> WeakList;
	int32 Index = INDEX_NONE;
	FString Path;
	if (!GameTestSelector::TakeUngeneratedEntry(WeakList, Index, Path))
	{
		return false;
	}
	UListView* List = WeakList.Get();
	if (List == nullptr)
	{
		return false;
	}
	const FString Key = FString::Printf(TEXT("%s#%d"), *Path, Index);
	if (InOutState.ScrolledEntries.Contains(Key))
	{
		return false;
	}
	InOutState.ScrolledEntries.Add(Key);
	List->ScrollIndexIntoView(Index);
	AddNote(InOutState, FString::Printf(TEXT("列表 %s 的第 %d 个条目没生成，自动滚到它"), *Path, Index));
	UE_LOG(LogGameTest, Display, TEXT("自动滚动：列表 %s 滚到第 %d 个条目"), *Path, Index);
	return true;
}

bool GameTestAutoScroll::ScrollIntoScrollBox(const TSharedPtr<SWidget>& InTarget, FState& InOutState)
{
	using namespace GameTestAutoScrollDetail;
	if (!InTarget.IsValid() || InOutState.ScrollBoxAttempts >= MaxScrollBoxAttempts)
	{
		return false;
	}
	UWidget* TargetWidget = GameTestSlate::FindOwningWidget(InTarget);
	if (TargetWidget == nullptr)
	{
		return false;
	}
	// 由里往外找第一个没把目标放进可见范围的滚动框：嵌套的滚动框先滚里层。
	for (TSharedPtr<SWidget> Current = InTarget->GetParentWidget(); Current.IsValid(); Current = Current->GetParentWidget())
	{
		UScrollBox* Box = Cast<UScrollBox>(GameTestSlate::FindSourceWidget(*Current));
		if (Box == nullptr)
		{
			continue;
		}
		if (IsInside(*InTarget, *Current))
		{
			continue;
		}
		++InOutState.ScrollBoxAttempts;
		Box->ScrollWidgetIntoView(TargetWidget, /*AnimateScroll=*/false);
		AddNote(InOutState, FString::Printf(TEXT("%s 在滚动框 %s 的可见范围外，自动滚过去"), *TargetWidget->GetName(), *Box->GetName()));
		UE_LOG(LogGameTest, Display, TEXT("自动滚动：滚动框 %s 滚到 %s"), *Box->GetName(), *TargetWidget->GetName());
		return true;
	}
	return false;
}

namespace GameTestAutoScrollScan
{
	using FScanBox = GameTestAutoScroll::FState::FScanBox;

	/** 真在屏上：有 Slate 控件、整条父链可见、有尺寸（对象池里藏着的、折叠的不算）。 */
	bool IsShown(UWidget* InWidget)
	{
		const TSharedPtr<SWidget> Slate = InWidget != nullptr ? InWidget->GetCachedWidget() : nullptr;
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

	/** 一个 UserWidget 自己树里的列表族（含 TileView）和滚动框；嵌套的 UserWidget 在外层遍历里单独算。 */
	void CollectFrom(UUserWidget* InWidget, TArray<UWidget*>& OutBoxes)
	{
		if (InWidget == nullptr || InWidget->WidgetTree == nullptr)
		{
			return;
		}
		InWidget->WidgetTree->ForEachWidget([&OutBoxes](UWidget* Widget)
		{
			if (Widget != nullptr && OutBoxes.Num() < GameTestAutoScrollDetail::MaxScanBoxes
				&& (Widget->IsA<UListView>() || Widget->IsA<UScrollBox>()) && IsShown(Widget))
			{
				OutBoxes.AddUnique(Widget);
			}
		});
	}

	/**
	 * 要翻的容器：游戏里**所有显示着的 UserWidget**（任何 UUserWidget 子类，不按名字认界面，用户 2026-11-18 定）里的列表和滚动框。
	 * 选择器写了屏、而且这个屏找得到时，它的容器排在前面先翻（多半就在那里）。
	 */
	void Collect(UUserWidget* InPreferred, TArray<UWidget*>& OutBoxes)
	{
		CollectFrom(InPreferred, OutBoxes);
		for (TObjectIterator<UUserWidget> It; It && OutBoxes.Num() < GameTestAutoScrollDetail::MaxScanBoxes; ++It)
		{
			UUserWidget* Widget = *It;
			if (!IsValid(Widget) || Widget->IsTemplate() || Widget == InPreferred)
			{
				continue;
			}
			// 编辑器自己的界面不在游戏世界里
			const UWorld* World = Widget->GetWorld();
			if (World == nullptr || !World->IsGameWorld() || !IsShown(Widget))
			{
				continue;
			}
			CollectFrom(Widget, OutBoxes);
		}
	}

	float ExtentOf(UWidget* InBox)
	{
		if (const UListView* List = Cast<UListView>(InBox))
		{
			return static_cast<float>(List->GetNumItems());
		}
		if (const UScrollBox* Box = Cast<UScrollBox>(InBox))
		{
			return Box->GetScrollOffsetOfEnd();
		}
		return 0.f;
	}

	/** 翻一页。到底了（没东西可翻、翻不动了、翻太多页）把 bDone 置上并返回 false。 */
	bool Step(FScanBox& InOutBox, UWidget* InBox)
	{
		using namespace GameTestAutoScrollDetail;
		if (InOutBox.Steps >= MaxScanSteps)
		{
			InOutBox.bDone = true;
			return false;
		}
		if (UListView* List = Cast<UListView>(InBox))
		{
			const int32 Num = List->GetNumItems();
			const int32 Shown = List->GetDisplayedEntryWidgets().Num();
			if (Num <= 0 || (!InOutBox.bStarted && Shown >= Num))
			{
				// 空列表、全都显示着：没什么可翻（条目数之后变了会重翻）
				InOutBox.bDone = true;
				return false;
			}
			if (!InOutBox.bStarted)
			{
				// 从头翻：录制时可能是往回滚找到的
				InOutBox.bStarted = true;
				InOutBox.LastOffset = 0.f;
				++InOutBox.Steps;
				List->ScrollToTop();
				return true;
			}
			// TileView 的位置按行算、显示的条目按个算，对不上：一次翻一行，到底只看「翻不动了」
			const bool bTile = List->IsA<UTileView>();
			const float Offset = List->GetScrollOffset();
			if ((!bTile && Offset + Shown >= Num) || (InOutBox.Steps > 1 && Offset <= InOutBox.LastOffset + 0.01f))
			{
				InOutBox.bDone = true;
				return false;
			}
			// 一页少一个，翻页前后留一个重叠，不会有条目夹在两页之间没生成
			const float Next = Offset + (bTile ? 1 : FMath::Max(1, Shown - 1));
			InOutBox.LastOffset = Offset;
			++InOutBox.Steps;
			List->SetScrollOffset(Next);
			return true;
		}
		if (UScrollBox* Box = Cast<UScrollBox>(InBox))
		{
			const float End = Box->GetScrollOffsetOfEnd();
			if (End <= 1.f)
			{
				InOutBox.bDone = true;
				return false;
			}
			if (!InOutBox.bStarted)
			{
				InOutBox.bStarted = true;
				InOutBox.LastOffset = 0.f;
				++InOutBox.Steps;
				Box->ScrollToStart();
				return true;
			}
			const float Offset = Box->GetScrollOffset();
			if (Offset >= End - 1.f || (InOutBox.Steps > 1 && Offset <= InOutBox.LastOffset + 0.5f))
			{
				InOutBox.bDone = true;
				return false;
			}
			// 不知道滚动框是横的还是竖的：按短边的八成翻，宁可多翻几页也不跳过内容
			const FVector2D Size = FVector2D(Box->GetCachedGeometry().GetLocalSize());
			const float Page = FMath::Max(40.f, static_cast<float>(FMath::Min(Size.X, Size.Y)) * 0.8f);
			InOutBox.LastOffset = Offset;
			++InOutBox.Steps;
			Box->SetScrollOffset(FMath::Min(End, Offset + Page));
			return true;
		}
		InOutBox.bDone = true;
		return false;
	}
}

FString GameTestAutoScroll::ScreenOf(const FString& InTarget)
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

void GameTestAutoScroll::MarkFound(FState& InOutState)
{
	InOutState.MissingSince = -1.0;
}

bool GameTestAutoScroll::ScanForMissing(const FString& InTarget, TMap<FString, TWeakObjectPtr<UWidget>>* InHandles,
	FState& InOutState, FString* InOutWhy)
{
	using namespace GameTestAutoScrollDetail;
	using namespace GameTestAutoScrollScan;
	const double Now = FPlatformTime::Seconds();
	if (InOutState.MissingSince < 0.0)
	{
		InOutState.MissingSince = Now;
	}
	if (Now - InOutState.MissingSince < ScanDelaySeconds
		|| (InOutState.LastScanAt >= 0.0 && Now - InOutState.LastScanAt < ScanIntervalSeconds))
	{
		return false;
	}
	// 选择器写了屏、屏也找得到时先翻它；没写、找不到也照样翻屏上所有 UserWidget 里的列表
	UUserWidget* Preferred = nullptr;
	const FString Screen = ScreenOf(InTarget);
	if (!Screen.IsEmpty())
	{
		TSharedPtr<FGameTestWidgetNode> ScreenNode;
		FString ScreenError;
		if (GameTestHandlers::TryResolveTarget(Screen + TEXT(":."), InHandles, ScreenNode, ScreenError) && ScreenNode.IsValid())
		{
			UWidget* ScreenWidget = ScreenNode->Widget.Get();
			Preferred = Cast<UUserWidget>(ScreenWidget);
			if (Preferred == nullptr && ScreenWidget != nullptr)
			{
				Preferred = ScreenWidget->GetTypedOuter<UUserWidget>();
			}
		}
	}
	TArray<UWidget*> Boxes;
	Collect(Preferred, Boxes);
	if (Boxes.Num() == 0)
	{
		// 屏上还没有列表（界面没打开）：照常等
		return false;
	}

	// 新出现的容器接到后面；翻完的长度变了（数据晚到、追加了条目）重翻
	for (UWidget* Box : Boxes)
	{
		FScanBox* Found = InOutState.ScanBoxes.FindByPredicate([Box](const FScanBox& Item) { return Item.Widget.Get() == Box; });
		const float Extent = ExtentOf(Box);
		if (Found == nullptr)
		{
			FScanBox& Added = InOutState.ScanBoxes.AddDefaulted_GetRef();
			Added.Widget = Box;
			Added.Extent = Extent;
			continue;
		}
		if (!FMath::IsNearlyEqual(Found->Extent, Extent, 0.5f))
		{
			const FString Name = Box->GetName();
			*Found = FScanBox();
			Found->Widget = Box;
			Found->Extent = Extent;
			if (InOutState.bScanEnded)
			{
				AddNote(InOutState, FString::Printf(TEXT("%s 的内容变了，重新翻"), *Name));
				InOutState.bScanEnded = false;
			}
		}
	}

	for (FScanBox& Item : InOutState.ScanBoxes)
	{
		UWidget* Box = Item.Widget.Get();
		if (Item.bDone || Box == nullptr || !Boxes.Contains(Box))
		{
			continue;
		}
		const bool bFirst = !Item.bStarted;
		if (Step(Item, Box))
		{
			InOutState.LastScanAt = Now;
			if (bFirst)
			{
				AddNote(InOutState, FString::Printf(TEXT("找不到目标，从头翻 %s 找"), *Box->GetName()));
				UE_LOG(LogGameTest, Display, TEXT("自动滚动：找不到 %s，从头翻 %s"), *InTarget, *Box->GetName());
			}
			if (InOutWhy != nullptr)
			{
				*InOutWhy += FString::Printf(TEXT("（正在翻 %s 找，第 %d 页）"), *Box->GetName(), Item.Steps);
			}
			return true;
		}
	}

	if (!InOutState.bScanEnded && InOutState.ScanBoxes.Num() > 0)
	{
		InOutState.bScanEnded = true;
		AddNote(InOutState, TEXT("界面上的列表、滚动框都翻到底了还没找到，原地等"));
		UE_LOG(LogGameTest, Display, TEXT("自动滚动：屏上的列表都翻到底了，还没找到 %s，原地等"), *InTarget);
	}
	if (InOutWhy != nullptr && InOutState.bScanEnded)
	{
		*InOutWhy += TEXT("（列表都翻到底了还没有，原地等）");
	}
	return false;
}
