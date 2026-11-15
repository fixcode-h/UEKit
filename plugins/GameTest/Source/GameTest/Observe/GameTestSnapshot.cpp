// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestSnapshot.h"

#include "Core/GameTestJson.h"
#include "Observe/GameTestWidgetJson.h"
#include "Observe/GameTestWidgetNode.h"
#include "Operate/GameTestOps.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/ExpandableArea.h"
#include "Components/InputKeySelector.h"
#include "Components/ListView.h"
#include "Components/MenuAnchor.h"
#include "Components/MultiLineEditableText.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/PanelWidget.h"
#include "Components/RichTextBlock.h"
#include "Components/ScrollBox.h"
#include "Components/Slider.h"
#include "Components/SpinBox.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "Components/WidgetSwitcher.h"
#include "JsonObjectConverter.h"
#include "UObject/UObjectIterator.h"

namespace
{
	/** 可选模块登记的状态读取器（CommonUI 的层级容器之类），按登记顺序问，第一个认的说了算。 */
	TArray<TPair<FDelegateHandle, GameTestSnapshot::FStateReader>>& GetStateReaders()
	{
		static TArray<TPair<FDelegateHandle, GameTestSnapshot::FStateReader>> Readers;
		return Readers;
	}

	FString ToFriendlyClassName(const UClass* InClass)
	{
		return GameTestSnapshot::FriendlyClassName(InClass);
	}

	FString ReadWidgetText(const UWidget* InWidget)
	{
		if (const UTextBlock* Text = Cast<UTextBlock>(InWidget))
		{
			return Text->GetText().ToString();
		}
		if (const URichTextBlock* RichText = Cast<URichTextBlock>(InWidget))
		{
			return RichText->GetText().ToString();
		}
		if (const UEditableText* Editable = Cast<UEditableText>(InWidget))
		{
			return Editable->GetText().ToString();
		}
		if (const UEditableTextBox* EditableBox = Cast<UEditableTextBox>(InWidget))
		{
			return EditableBox->GetText().ToString();
		}
		if (const UMultiLineEditableText* MultiText = Cast<UMultiLineEditableText>(InWidget))
		{
			return MultiText->GetText().ToString();
		}
		if (const UMultiLineEditableTextBox* MultiBox = Cast<UMultiLineEditableTextBox>(InWidget))
		{
			return MultiBox->GetText().ToString();
		}
		return FString();
	}

	/**
	 * 声明在这个类**自己身上**的属性（不含继承来的引擎属性）——C# 的 `[UProperty]` 也走这条通道。
	 * 用 `ExcludeSuper` 过滤继承属性：`UUserWidget` 自己有上百个属性，全导出来只会盖住真内容。
	 */
	void FillDeclaredProperties(const UWidget& InWidget, FGameTestWidgetNode& OutNode)
	{
		const UClass* WidgetClass = InWidget.GetClass();
		if (WidgetClass == nullptr)
		{
			return;
		}

		const TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		UWidget* MutableWidget = const_cast<UWidget*>(&InWidget);

		for (TFieldIterator<FProperty> It(WidgetClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			const FProperty* Property = *It;
			if (Property == nullptr || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}

			FString Value;
			Property->ExportTextItem_Direct(
				Value,
				Property->ContainerPtrToValuePtr<void>(MutableWidget),
				/*DefaultValue=*/nullptr,
				MutableWidget,
				PPF_None);

			Props->SetStringField(Property->GetName(), Value);
		}

		if (Props->Values.Num() > 0)
		{
			OutNode.Props = Props;
		}
	}

	/** 有状态控件的当前值——AI 靠它读回"操作生效没有"。 */
	void FillState(const UWidget& InWidget, FGameTestWidgetNode& OutNode)
	{
		const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();

		if (const UCheckBox* CheckBox = Cast<UCheckBox>(&InWidget))
		{
			State->SetBoolField(TEXT("checked"), CheckBox->IsChecked());
		}
		else if (const USlider* Slider = Cast<USlider>(&InWidget))
		{
			State->SetNumberField(TEXT("value"), Slider->GetValue());
		}
		else if (const USpinBox* SpinBox = Cast<USpinBox>(&InWidget))
		{
			State->SetNumberField(TEXT("value"), SpinBox->GetValue());
		}
		else if (const UComboBoxString* ComboBox = Cast<UComboBoxString>(&InWidget))
		{
			State->SetStringField(TEXT("option"), ComboBox->GetSelectedOption());
			State->SetNumberField(TEXT("index"), ComboBox->GetSelectedIndex());
		}
		else if (const UExpandableArea* Area = Cast<UExpandableArea>(&InWidget))
		{
			State->SetBoolField(TEXT("expanded"), Area->GetIsExpanded());
		}
		else if (const UScrollBox* ScrollBox = Cast<UScrollBox>(&InWidget))
		{
			// 滚动的读回：引擎的滚动请求在下一帧布局时才生效，所以要隔一帧再来快照才看得到位移。
			State->SetNumberField(TEXT("offset"), ScrollBox->GetScrollOffset());
			State->SetNumberField(TEXT("offsetOfEnd"), ScrollBox->GetScrollOffsetOfEnd());
		}
		else if (const UWidgetSwitcher* Switcher = Cast<UWidgetSwitcher>(&InWidget))
		{
			State->SetNumberField(TEXT("activeIndex"), Switcher->GetActiveWidgetIndex());
			State->SetNumberField(TEXT("numWidgets"), Switcher->GetNumWidgets());
		}
		else if (const UListView* List = Cast<UListView>(&InWidget))
		{
			// 引擎把"条目类不是蓝图"的列表整块换成一行错误文本时 slateList=false——
			// 那时列表仍有数据项却选不中也读不回，AI 在快照里就该看见这一点。
			State->SetBoolField(TEXT("slateList"), GameTestOps::HasLiveSlateList(*List));
			State->SetNumberField(TEXT("selectedCount"), List->GetNumItemsSelected());
			// 已生成的条目控件数：树折叠时只有根、展开后子条目才生成，所以它是"展开生效"的可观测量。
			State->SetNumberField(TEXT("displayedEntries"), List->GetDisplayedEntryWidgets().Num());
		}
		else if (const UMenuAnchor* MenuAnchor = Cast<UMenuAnchor>(&InWidget))
		{
			State->SetBoolField(TEXT("open"), MenuAnchor->IsOpen());
		}
		else if (const UInputKeySelector* KeySelector = Cast<UInputKeySelector>(&InWidget))
		{
			State->SetStringField(TEXT("key"), KeySelector->GetSelectedKey().Key.GetFName().ToString());
		}
		else if (!GameTestSnapshot::ReadExtraState(InWidget, *State))
		{
			return;
		}

		OutNode.State = State;
	}

	void FillListItems(UListView& InList, FGameTestWidgetNode& OutNode)
	{
		const TArray<UObject*>& Items = InList.GetListItems();
		// BP_GetSelectedItem 是 private（UListView 的 Blueprint 包装带 AllowPrivateAccess）；
		// 公开的那条是 ITypedUMGListView<UObject*> 上"与 SListView 对齐"的 API。
		const UObject* Selected = InList.GetSelectedItem();

		OutNode.Items.Reserve(Items.Num());
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			const UObject* Item = Items[Index];

			const TSharedPtr<FGameTestListItem> Entry = MakeShared<FGameTestListItem>();
			Entry->Index = Index;
			Entry->Item = Items[Index];
			Entry->ClassName = Item != nullptr ? ToFriendlyClassName(Item->GetClass()) : FString();
			Entry->Properties = GameTestSnapshot::DumpProperties(Item);
			Entry->bSelected = (Item != nullptr && Item == Selected);
			OutNode.Items.Add(Entry);
		}
	}

	/** 哪些控件当前持有 Slate 焦点——"点了没反应"最常见的三个原因之一。 */
	bool IsFocused(const UWidget& InWidget)
	{
		const TSharedPtr<SWidget> Cached = InWidget.GetCachedWidget();
		return Cached.IsValid() && Cached->HasAnyUserFocus();
	}
}

TSharedPtr<FJsonObject> GameTestSnapshot::DumpProperties(const UObject* InObject)
{
	if (InObject == nullptr)
	{
		return nullptr;
	}

	FString Dumped;
	if (!FJsonObjectConverter::UStructToJsonObjectString(
			InObject->GetClass(),
			InObject,
			Dumped,
			/*CheckFlags=*/0,
			/*SkipFlags=*/CPF_Transient | CPF_Deprecated))
	{
		return nullptr;
	}

	FString ParseError;
	return GameTestJson::ParseObjectFromText(Dumped, ParseError);
}

TSharedPtr<FGameTestWidgetNode> GameTestSnapshot::CaptureSubtree(UWidget* InWidget, const FString& InPath, bool InIsScreenRoot, bool bInLight)
{
	if (InWidget == nullptr)
	{
		return nullptr;
	}

	const TSharedPtr<FGameTestWidgetNode> Node = MakeShared<FGameTestWidgetNode>();
	Node->Name = InWidget->GetFName().ToString();
	Node->ClassName = ToFriendlyClassName(InWidget->GetClass());
	Node->Path = InPath.IsEmpty() ? Node->Name : InPath + TEXT("/") + Node->Name;
	Node->Widget = InWidget;

	Node->Visibility = StaticEnum<ESlateVisibility>()->GetNameStringByValue(static_cast<int64>(InWidget->GetVisibility()));
	Node->bEnabled = InWidget->GetIsEnabled();
	Node->bFocused = IsFocused(*InWidget);
	Node->Text = ReadWidgetText(InWidget);

	if (InWidget->GetCachedWidget().IsValid())
	{
		const FGeometry& Geometry = InWidget->GetCachedGeometry();
		const UE::Slate::FDeprecateVector2DResult AbsolutePosition = Geometry.GetAbsolutePosition();
		const UE::Slate::FDeprecateVector2DResult AbsoluteSize = Geometry.GetAbsoluteSize();
		Node->Position = FVector2D(AbsolutePosition.X, AbsolutePosition.Y);
		Node->Size = FVector2D(AbsoluteSize.X, AbsoluteSize.Y);
		Node->bHasGeometry = true;
	}

	if (UListView* List = Cast<UListView>(InWidget))
	{
		FillListItems(*List, *Node);
	}

	FillState(*InWidget, *Node);
	if (!bInLight)
	{
		FillDeclaredProperties(*InWidget, *Node);
	}

	if (const UUserWidget* UserWidget = Cast<UUserWidget>(InWidget))
	{
		Node->bIsUserWidget = true;
		Node->NestedScreenClass = ToFriendlyClassName(UserWidget->GetClass());

		// 屏根本身也是 UUserWidget：这时要展开**它自己的**控件树（树的入口是 WidgetTree->RootWidget），
		// 否则每个屏都会变成"只有一个节点"的空壳——AI 拿到一张没有控件的清单。
		if (!InIsScreenRoot)
		{
			// 子界面自成一棵屏：这里只记引用，不把它整棵吞进父树，
			// 否则同一个控件会同时出现在两棵树里，选择器的"唯一命中"就不再成立。
			return Node;
		}

		if (const UWidgetTree* Tree = UserWidget->WidgetTree)
		{
			if (UWidget* TreeRoot = Tree->RootWidget)
			{
				if (const TSharedPtr<FGameTestWidgetNode> Child = CaptureSubtree(TreeRoot, Node->Path, /*InIsScreenRoot=*/false, bInLight))
				{
					Node->Children.Add(Child);
				}
			}
		}

		return Node;
	}

	if (const UPanelWidget* Panel = Cast<UPanelWidget>(InWidget))
	{
		const int32 ChildCount = Panel->GetChildrenCount();
		Node->Children.Reserve(ChildCount);
		for (int32 Index = 0; Index < ChildCount; ++Index)
		{
			if (const TSharedPtr<FGameTestWidgetNode> Child = CaptureSubtree(Panel->GetChildAt(Index), Node->Path, /*InIsScreenRoot=*/false, bInLight))
			{
				Node->Children.Add(Child);
			}
		}
	}

	return Node;
}

void GameTestSnapshot::CaptureScreens(UWorld* InWorld, TArray<TSharedPtr<FGameTestWidgetNode>>& OutScreens, bool bInLight)
{
	OutScreens.Reset();
	if (InWorld == nullptr)
	{
		return;
	}

	for (TObjectIterator<UUserWidget> Iterator; Iterator; ++Iterator)
	{
		UUserWidget* Widget = *Iterator;
		if (!IsValid(Widget) || Widget->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
		{
			continue;
		}

		// 编辑器自己的界面住在编辑器世界里，按世界过滤就天然排除了它们。
		if (Widget->GetWorld() != InWorld)
		{
			continue;
		}

		if (!Widget->GetCachedWidget().IsValid())
		{
			continue;
		}

		if (const TSharedPtr<FGameTestWidgetNode> Screen = CaptureSubtree(Widget, FString(), /*InIsScreenRoot=*/true, bInLight))
		{
			OutScreens.Add(Screen);
		}
	}

	// 顺序稳定：两次快照的数组顺序一致，AI 读到的差异才是真差异。
	OutScreens.Sort([](const TSharedPtr<FGameTestWidgetNode>& InLeft, const TSharedPtr<FGameTestWidgetNode>& InRight)
	{
		return InLeft->ClassName < InRight->ClassName;
	});
}

TSharedPtr<FJsonObject> GameTestSnapshot::ScreensToJson(
	const TArray<TSharedPtr<FGameTestWidgetNode>>& InScreens,
	bool bInIncludeGeometry,
	bool bInIncludeProps,
	int32 InMaxNodes)
{
	const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("screenCount"), InScreens.Num());

	TArray<TSharedPtr<FJsonValue>> ScreenValues;
	ScreenValues.Reserve(InScreens.Num());

	int32 NodeCount = 0;
	for (const TSharedPtr<FGameTestWidgetNode>& Screen : InScreens)
	{
		if (!Screen.IsValid())
		{
			continue;
		}

		ScreenValues.Add(MakeShared<FJsonValueObject>(
			GameTestWidgetJson::ToJson(*Screen, bInIncludeGeometry, bInIncludeProps, InMaxNodes, NodeCount)));
	}

	Root->SetArrayField(TEXT("screens"), ScreenValues);
	Root->SetNumberField(TEXT("nodeCount"), NodeCount);
	return Root;
}

FString GameTestSnapshot::FriendlyClassName(const UClass* InClass)
{
	// 去掉蓝图生成类的 `_C` 后缀：AI 写选择器时用的是资产名，不是类名。
	FString Name = InClass != nullptr ? InClass->GetName() : FString();
	Name.RemoveFromEnd(TEXT("_C"));
	return Name;
}

FDelegateHandle GameTestSnapshot::AddStateReader(FStateReader InReader)
{
	const FDelegateHandle Handle(FDelegateHandle::GenerateNewHandle);
	GetStateReaders().Emplace(Handle, MoveTemp(InReader));
	return Handle;
}

void GameTestSnapshot::RemoveStateReader(FDelegateHandle InHandle)
{
	GetStateReaders().RemoveAll([InHandle](const TPair<FDelegateHandle, FStateReader>& Pair) { return Pair.Key == InHandle; });
}

bool GameTestSnapshot::ReadExtraState(const UWidget& InWidget, FJsonObject& OutState)
{
	for (const TPair<FDelegateHandle, FStateReader>& Pair : GetStateReaders())
	{
		if (Pair.Value(InWidget, OutState))
		{
			return true;
		}
	}
	return false;
}
