// Copyright Epic Games, Inc. All Rights Reserved.

#include "Probe/GameTestProbeListWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/ListView.h"
#include "Components/ListViewBase.h"
#include "Components/TextBlock.h"

namespace
{
	/** 条目里那个文本控件的名字：纯 C++ 与 WBP 两条路径共用这一个名字。 */
	const FName ProbeEntryLabelName = TEXT("Label");
}

UClass* GameTestProbe::ResolveEntryClass()
{
	// 只用插件里的 C++ 条目类，不依赖项目里的任何资产：探针是自带的测试靶子，
	// 换个项目也要能直接用。
	return UGameTestProbeEntryWidget::StaticClass();
}

UGameTestProbeEntryWidget::UGameTestProbeEntryWidget(const FObjectInitializer& InObjectInitializer)
	: Super(InObjectInitializer)
{
}

TSharedRef<SWidget> UGameTestProbeEntryWidget::RebuildWidget()
{
	// 纯 C++ 的条目控件没有 WBP，自己造一棵只有一个文本的树；WBP 子类用它资产里那棵。
	if (WidgetTree != nullptr && WidgetTree->RootWidget == nullptr)
	{
		Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), ProbeEntryLabelName);
		WidgetTree->RootWidget = Label;
	}

	return Super::RebuildWidget();
}

void UGameTestProbeEntryWidget::NativeOnListItemObjectSet(UObject* InListItemObject)
{
	IUserObjectListEntry::NativeOnListItemObjectSet(InListItemObject);

	// WBP 子类的控件树来自资产、成员是空的，所以也按名字找一遍——两条路径共用这一份。
	UTextBlock* Target = Label;
	if (Target == nullptr && WidgetTree != nullptr)
	{
		Target = Cast<UTextBlock>(WidgetTree->FindWidget(ProbeEntryLabelName));
	}

	if (Target != nullptr)
	{
		const UGameTestProbeItem* Item = Cast<UGameTestProbeItem>(InListItemObject);
		Target->SetText(Item != nullptr ? FText::FromString(Item->DisplayName) : FText::GetEmpty());
	}
}

TSharedRef<SWidget> UGameTestProbeListWidget::RebuildWidget()
{
	if (WidgetTree != nullptr && WidgetTree->RootWidget == nullptr)
	{
		UListView* List = WidgetTree->ConstructWidget<UListView>(UListView::StaticClass(), TEXT("ProbeListView"));
		WidgetTree->RootWidget = List;
		ListView = List;

		// 引擎没给 EntryWidgetClass 的公开 setter（只有 protected 成员 + 设计器里配），
		// 而纯 C++ 造的列表必须在运行时定条目类，否则刷新时会拿空类去建行。按属性名写进去。
		if (FProperty* EntryClassProperty = FindFProperty<FProperty>(UListViewBase::StaticClass(), TEXT("EntryWidgetClass")))
		{
			UClass* EntryClass = GameTestProbe::ResolveEntryClass();
			EntryClassProperty->SetValue_InContainer(List, &EntryClass);
		}
	}

	// 建完就播种：TakeWidget 会走这里，比 NativeOnInitialized 更早且只走一次。
	SeedProbeItems();

	return Super::RebuildWidget();
}

void UGameTestProbeListWidget::SeedProbeItems()
{
	if (ListView == nullptr)
	{
		return;
	}

	ListView->ClearListItems();
	for (int32 Index = 0; Index < 3; ++Index)
	{
		UGameTestProbeItem* Item = NewObject<UGameTestProbeItem>(this);
		Item->Id = Index + 1;
		Item->DisplayName = FString::Printf(TEXT("条目%d"), Index + 1);
		ListView->AddItem(Item);
	}
}
