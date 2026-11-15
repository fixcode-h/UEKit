// Copyright Epic Games, Inc. All Rights Reserved.

#include "Probe/GameTestProbeContainers.h"

#include "Blueprint/WidgetTree.h"
#include "Components/ListViewBase.h"
#include "Components/TextBlock.h"
#include "Components/TileView.h"
#include "Components/TreeView.h"
#include "Widgets/CommonActivatableWidgetContainer.h"

namespace
{
	/** 给列表族定条目类：引擎没给公开 setter，按属性名写进去（与 ListView 探针同款）。 */
	void ApplyProbeEntryClass(UListViewBase* InList)
	{
		if (InList == nullptr)
		{
			return;
		}

		if (FProperty* EntryClassProperty = FindFProperty<FProperty>(UListViewBase::StaticClass(), TEXT("EntryWidgetClass")))
		{
			UClass* EntryClass = GameTestProbe::ResolveEntryClass();
			EntryClassProperty->SetValue_InContainer(InList, &EntryClass);
		}
	}

	/** 造一个探针条目（外层用给定对象，避免不同探针之间互相持有）。 */
	UGameTestProbeItem* MakeProbeItem(UObject* InOuter, int32 InId, const FString& InDisplayName)
	{
		UGameTestProbeItem* Item = NewObject<UGameTestProbeItem>(InOuter);
		Item->Id = InId;
		Item->DisplayName = InDisplayName;
		return Item;
	}
}

//~ TileView ----------------------------------------------------------------

TSharedRef<SWidget> UGameTestProbeTileWidget::RebuildWidget()
{
	if (WidgetTree != nullptr && WidgetTree->RootWidget == nullptr)
	{
		UTileView* Tile = WidgetTree->ConstructWidget<UTileView>(UTileView::StaticClass(), TEXT("ProbeTileView"));
		WidgetTree->RootWidget = Tile;
		TileView = Tile;

		ApplyProbeEntryClass(Tile);
	}

	SeedProbeItems();
	return Super::RebuildWidget();
}

void UGameTestProbeTileWidget::SeedProbeItems()
{
	if (TileView == nullptr)
	{
		return;
	}

	TileView->ClearListItems();
	for (int32 Index = 0; Index < 6; ++Index)
	{
		TileView->AddItem(MakeProbeItem(this, Index + 1, FString::Printf(TEXT("条目%d"), Index + 1)));
	}
}

//~ TreeView ----------------------------------------------------------------

TSharedRef<SWidget> UGameTestProbeTreeWidget::RebuildWidget()
{
	if (WidgetTree != nullptr && WidgetTree->RootWidget == nullptr)
	{
		UTreeView* Tree = WidgetTree->ConstructWidget<UTreeView>(UTreeView::StaticClass(), TEXT("ProbeTreeView"));
		WidgetTree->RootWidget = Tree;
		TreeView = Tree;

		ApplyProbeEntryClass(Tree);
		// 取子委托要在展开之前绑好：STreeView 靠它把子项挂到根下面。
		Tree->SetOnGetItemChildren(this, &UGameTestProbeTreeWidget::HandleGetItemChildren);
	}

	SeedProbeItems();
	return Super::RebuildWidget();
}

void UGameTestProbeTreeWidget::SeedProbeItems()
{
	if (TreeView == nullptr)
	{
		return;
	}

	TreeView->ClearListItems();
	RootItems.Reset();

	for (int32 RootIndex = 0; RootIndex < 2; ++RootIndex)
	{
		UGameTestProbeTreeItem* Root = NewObject<UGameTestProbeTreeItem>(this);
		Root->Id = RootIndex + 1;
		Root->DisplayName = FString::Printf(TEXT("分组%d"), RootIndex + 1);

		for (int32 ChildIndex = 0; ChildIndex < 2; ++ChildIndex)
		{
			UGameTestProbeItem* Child = MakeProbeItem(
				this,
				(RootIndex + 1) * 10 + ChildIndex + 1,
				FString::Printf(TEXT("子%d.%d"), RootIndex + 1, ChildIndex + 1));
			Root->Children.Add(Child);
		}

		RootItems.Add(Root);
		TreeView->AddItem(Root);
	}
}

void UGameTestProbeTreeWidget::HandleGetItemChildren(UObject* InItem, TArray<UObject*>& OutChildren)
{
	OutChildren.Reset();

	if (const UGameTestProbeTreeItem* TreeItem = Cast<UGameTestProbeTreeItem>(InItem))
	{
		for (const TObjectPtr<UGameTestProbeItem>& Child : TreeItem->Children)
		{
			if (Child != nullptr)
			{
				OutChildren.Add(Child);
			}
		}
	}
}

//~ CommonUI 层级栈 -----------------------------------------------------------

TSharedRef<SWidget> UGameTestProbeStackWidget::RebuildWidget()
{
	if (WidgetTree != nullptr && WidgetTree->RootWidget == nullptr)
	{
		UCommonActivatableWidgetStack* NewStack =
			WidgetTree->ConstructWidget<UCommonActivatableWidgetStack>(UCommonActivatableWidgetStack::StaticClass(), TEXT("ProbeStack"));
		WidgetTree->RootWidget = NewStack;
		Stack = NewStack;
	}

	return Super::RebuildWidget();
}

TSharedRef<SWidget> UGameTestProbeScreen::RebuildWidget()
{
	if (WidgetTree != nullptr && WidgetTree->RootWidget == nullptr)
	{
		Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Label"));
		Label->SetText(FText::FromString(GetClass()->GetName()));
		WidgetTree->RootWidget = Label;
	}

	return Super::RebuildWidget();
}
