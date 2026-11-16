// Copyright Epic Games, Inc. All Rights Reserved.

#include "Operate/GameTestOps.h"
#include "GameTestCompat.h"

#include "Core/GameTestJson.h"

#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
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
#include "Components/ScrollBox.h"
#include "Components/Slider.h"
#include "Components/SpinBox.h"
#include "Components/TreeView.h"
#include "Components/Widget.h"
#include "Components/WidgetSwitcher.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"


namespace
{
	/** 参数白名单：出现别的字段就是写错了，直接报出来。 */
	bool RejectUnknownParams(const FJsonObject& InParams, const TArray<FString>& InAllowed, FString& OutMessage)
	{
		for (const auto& Pair : InParams.Values)
		{
			const FString PairKey = GAMETEST_JSON_KEY(Pair.Key);
			if (PairKey == TEXT("target") || PairKey == TEXT("action") || PairKey == TEXT("note")
				|| PairKey == TEXT("timeoutMs") || PairKey == TEXT("onFail"))
			{
				continue;
			}

			if (!InAllowed.Contains(PairKey))
			{
				OutMessage = FString::Printf(TEXT("这个控件不支持字段 '%s'（可用：%s）"),
					*PairKey, InAllowed.Num() > 0 ? *FString::Join(InAllowed, TEXT(", ")) : TEXT("无"));
				return false;
			}
		}
		return true;
	}

	void SetDelegateFiredFlag(const TSharedPtr<FJsonObject>& InPayload, const TCHAR* InName, bool bInFired)
	{
		if (InPayload.IsValid())
		{
			InPayload->SetBoolField(InName, bInFired);
		}
	}

	/** 目标不可点时的提示：子树里找得到可点的控件就把路径报出来。 */
	void DescribeClickableInSubtree(const UWidget& InWidget, FString& OutHint)
	{
		if (const UPanelWidget* Panel = Cast<UPanelWidget>(&InWidget))
		{
			for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
			{
				if (UWidget* Child = Panel->GetChildAt(Index))
				{
					FString Hint;
					if (GameTestOps::IsClickable(Child, Hint))
					{
						OutHint = FString::Printf(TEXT("；它下面有可点的 %s（%s）"),
							*Child->GetFName().ToString(), *Hint);
						return;
					}
					DescribeClickableInSubtree(*Child, OutHint);
					if (!OutHint.IsEmpty())
					{
						return;
					}
				}
			}
		}
	}
}

bool GameTestOps::IsClickable(const UWidget* InWidget, FString& OutHint)
{
	if (InWidget == nullptr)
	{
		return false;
	}

	if (InWidget->IsA<UButton>())
	{
		OutHint = TEXT("按钮：走 SButton::ExecuteOnClick");
		return true;
	}
	if (InWidget->IsA<UCheckBox>())
	{
		OutHint = TEXT("复选：走 SlateOnCheckStateChangedCallback（切换状态并广播）");
		return true;
	}
	if (InWidget->IsA<UMenuAnchor>())
	{
		OutHint = TEXT("菜单锚：走 Open/Close/ToggleOpen");
		return true;
	}
	if (InWidget->IsA<UListView>())
	{
		OutHint = TEXT("列表：用 select 选条目，不要 click 列表本身");
		return false;
	}
	if (InWidget->IsA<UExpandableArea>())
	{
		OutHint = TEXT("展开器：用 set expanded=true/false");
		return false;
	}
	if (InWidget->IsA<UComboBoxString>())
	{
		OutHint = TEXT("下拉框：用 set option=... 或 index=...");
		return false;
	}

	OutHint = FString::Printf(TEXT("类 %s 没有代码级点击入口"), *InWidget->GetClass()->GetName());
	return false;
}

bool GameTestOps::Click(UWidget* InWidget, FString& OutMessage, TSharedPtr<FJsonObject>& OutPayload)
{
	if (InWidget == nullptr)
	{
		OutMessage = TEXT("目标控件已失效");
		return false;
	}

	if (UButton* Button = Cast<UButton>(InWidget))
	{
		// 一次真实点击的委托顺序：按下 → 点击 → 抬起。三个都发，游戏/C# 绑在哪一个都能收到。
		Button->OnPressed.Broadcast();
		Button->OnClicked.Broadcast();
		Button->OnReleased.Broadcast();

		OutMessage = TEXT("已点击（UButton::OnPressed → OnClicked → OnReleased）");
		return true;
	}

	if (UCheckBox* CheckBox = Cast<UCheckBox>(InWidget))
	{
		const bool bNewState = !CheckBox->IsChecked();
		// SetIsChecked 只改状态 + 通知字段，不广播 OnCheckStateChanged（引擎把广播留在 Slate 回调里），
		// 所以这里补一次——否则"代码级勾选"与"人点一下"对游戏逻辑不等价。
		CheckBox->SetIsChecked(bNewState);
		CheckBox->OnCheckStateChanged.Broadcast(bNewState);

		OutMessage = FString::Printf(TEXT("已切换复选状态 → %s（并广播 OnCheckStateChanged）"),
			bNewState ? TEXT("true") : TEXT("false"));
		return true;
	}

	if (UMenuAnchor* MenuAnchor = Cast<UMenuAnchor>(InWidget))
	{
		MenuAnchor->ToggleOpen(/*bFocusOnOpen=*/false);
		OutMessage = FString::Printf(TEXT("已切换菜单开合 → %s"), MenuAnchor->IsOpen() ? TEXT("open") : TEXT("closed"));
		SetDelegateFiredFlag(OutPayload, TEXT("open"), MenuAnchor->IsOpen());
		return true;
	}

	FString Hint;
	IsClickable(InWidget, Hint);
	FString SubtreeHint;
	DescribeClickableInSubtree(*InWidget, SubtreeHint);

	OutMessage = FString::Printf(TEXT("%s 不可点：%s%s"), *InWidget->GetFName().ToString(), *Hint, *SubtreeHint);
	return false;
}

bool GameTestOps::SetText(UWidget* InWidget, const FString& InText, bool bInCommit, FString& OutMessage, TSharedPtr<FJsonObject>& OutPayload)
{
	if (InWidget == nullptr)
	{
		OutMessage = TEXT("目标控件已失效");
		return false;
	}

	const FText Text = FText::FromString(InText);

	if (UEditableText* Editable = Cast<UEditableText>(InWidget))
	{
		Editable->SetText(Text);
		Editable->OnTextChanged.Broadcast(Text);
		if (bInCommit)
		{
			Editable->OnTextCommitted.Broadcast(Text, ETextCommit::OnUserMovedFocus);
		}
		OutMessage = FString::Printf(TEXT("已写入文本（%s）"), bInCommit ? TEXT("并提交") : TEXT("未提交"));
		SetDelegateFiredFlag(OutPayload, TEXT("committed"), bInCommit);
		return true;
	}

	if (UEditableTextBox* EditableBox = Cast<UEditableTextBox>(InWidget))
	{
		EditableBox->SetText(Text);
		EditableBox->OnTextChanged.Broadcast(Text);
		if (bInCommit)
		{
			EditableBox->OnTextCommitted.Broadcast(Text, ETextCommit::OnUserMovedFocus);
		}
		OutMessage = FString::Printf(TEXT("已写入文本（%s）"), bInCommit ? TEXT("并提交") : TEXT("未提交"));
		SetDelegateFiredFlag(OutPayload, TEXT("committed"), bInCommit);
		return true;
	}

	if (UMultiLineEditableText* MultiText = Cast<UMultiLineEditableText>(InWidget))
	{
		MultiText->SetText(Text);
		MultiText->OnTextChanged.Broadcast(Text);
		if (bInCommit)
		{
			MultiText->OnTextCommitted.Broadcast(Text, ETextCommit::OnUserMovedFocus);
		}
		OutMessage = FString::Printf(TEXT("已写入多行文本（%s）"), bInCommit ? TEXT("并提交") : TEXT("未提交"));
		SetDelegateFiredFlag(OutPayload, TEXT("committed"), bInCommit);
		return true;
	}

	if (UMultiLineEditableTextBox* MultiBox = Cast<UMultiLineEditableTextBox>(InWidget))
	{
		MultiBox->SetText(Text);
		MultiBox->OnTextChanged.Broadcast(Text);
		if (bInCommit)
		{
			MultiBox->OnTextCommitted.Broadcast(Text, ETextCommit::OnUserMovedFocus);
		}
		OutMessage = FString::Printf(TEXT("已写入多行文本（%s）"), bInCommit ? TEXT("并提交") : TEXT("未提交"));
		SetDelegateFiredFlag(OutPayload, TEXT("committed"), bInCommit);
		return true;
	}

	OutMessage = FString::Printf(TEXT("%s（%s）不是输入框；输入框类：EditableText / EditableTextBox / MultiLineEditableText / MultiLineEditableTextBox"),
		*InWidget->GetFName().ToString(), *InWidget->GetClass()->GetName());
	return false;
}

bool GameTestOps::HasLiveSlateList(const UListView& InList)
{
	const TSharedPtr<SWidget> Cached = InList.GetCachedWidget();

	// 判"坏"而不是判"好"：引擎的错误路径只有一个形状——整块换成一行 `STextBlock`。
	// 反向（比对 Slate 类型名找 SListView）会**永远判假**：类型名是 SNew 的字面量，
	// UMG 的列表由 `SNew(ListViewT<ItemType>)` 造出，字符串就是 "ListViewT<ItemType>"
	// （实测踩过：列表工作正常时 slateList 仍报 false）。
	return Cached.IsValid() && Cached->GetType() != TEXT("STextBlock");
}

bool GameTestOps::SelectListItem(UListView* InList, int32 InIndex, bool bInAdd, FString& OutMessage, TSharedPtr<FJsonObject>& OutPayload)
{
	if (InList == nullptr)
	{
		OutMessage = TEXT("目标不是列表族（ListView / TileView / TreeView）；选中条目只能用在它们上");
		return false;
	}

	const int32 NumItems = InList->GetNumItems();
	if (!InList->GetListItems().IsValidIndex(InIndex))
	{
		OutMessage = FString::Printf(TEXT("条目索引 %d 越界：列表里有 %d 项"), InIndex, NumItems);
		return false;
	}

	UObject* Item = InList->GetItemAt(InIndex);
	if (Item == nullptr)
	{
		OutMessage = FString::Printf(TEXT("索引 %d 上没有数据项"), InIndex);
		return false;
	}

	if (bInAdd)
	{
		InList->SetItemSelection(Item, /*bIsSelected=*/true, ESelectInfo::Direct);
	}
	else
	{
		InList->SetSelectedItem(Item);
	}

	// 读回校验：这条链最终落到 SListView，而它有一类**静默无效**的状态——最典型的是
	// 编辑器里条目类不是蓝图（见 HasLiveSlateList 的说明）。不校验就会把"什么都没发生"报成成功，
	// 于是缺陷在下游以"UI 没反应"的面目出现，最难查。
	if (InList->GetSelectedItem() != Item)
	{
		OutMessage = FString::Printf(
			TEXT("选中没有生效：已对第 %d 项调用选中接口，读回的选中项却不是它。%s"),
			InIndex,
			HasLiveSlateList(*InList)
				? TEXT("列表有 Slate 列表控件却没接受这次选中——把失败消息与 snapshot 里的条目状态一起看")
				: TEXT("这个列表没有活的 Slate 列表控件（编辑器里 EntryWidgetClass 不是蓝图时引擎会把列表整块换成一行错误文本），选中与读回都会静默失效"));
		return false;
	}

	OutPayload->SetNumberField(TEXT("index"), InIndex);
	OutPayload->SetStringField(TEXT("itemClass"), Item->GetClass()->GetName());
	OutMessage = FString::Printf(TEXT("已选中第 %d 项（%s）"), InIndex, *Item->GetClass()->GetName());
	return true;
}

bool GameTestOps::Scroll(
	UWidget* InWidget,
	const FString& InMode,
	double InValue,
	int32 InIndex,
	UWidget* InTargetWidget,
	FString& OutMessage,
	TSharedPtr<FJsonObject>& OutPayload)
{
	if (InWidget == nullptr)
	{
		OutMessage = TEXT("目标控件已失效");
		return false;
	}

	if (UScrollBox* ScrollBox = Cast<UScrollBox>(InWidget))
	{
		if (InMode == TEXT("offset"))
		{
			ScrollBox->SetScrollOffset(static_cast<float>(InValue));
		}
		else if (InMode == TEXT("start"))
		{
			ScrollBox->ScrollToStart();
		}
		else if (InMode == TEXT("end"))
		{
			ScrollBox->ScrollToEnd();
		}
		else if (InMode == TEXT("widget"))
		{
			if (InTargetWidget == nullptr)
			{
				OutMessage = TEXT("mode=widget 需要 widget 字段指定要滚到哪个控件");
				return false;
			}
			ScrollBox->ScrollWidgetIntoView(InTargetWidget, /*AnimateScroll=*/false);
		}
		else
		{
			OutMessage = FString::Printf(TEXT("ScrollBox 不认识 mode '%s'（可用：offset / start / end / widget）"), *InMode);
			return false;
		}

		OutPayload->SetNumberField(TEXT("offset"), ScrollBox->GetScrollOffset());
		OutPayload->SetNumberField(TEXT("offsetOfEnd"), ScrollBox->GetScrollOffsetOfEnd());
		// 引擎的滚动请求（SetScrollOffset / ScrollToStart / ScrollToEnd）都在**下一次布局**时生效，
		// 所以这里立刻读回拿到的还是旧值——把这件事写进消息，别让调用方以为滚动没生效。
		OutMessage = FString::Printf(TEXT("已请求滚动（下一帧布局时生效）→ 当前 offset=%.1f（末端 %.1f）"),
			ScrollBox->GetScrollOffset(), ScrollBox->GetScrollOffsetOfEnd());
		return true;
	}

	if (UListView* List = Cast<UListView>(InWidget))
	{
		if (InMode != TEXT("index"))
		{
			OutMessage = FString::Printf(TEXT("ListView 只支持 mode=index（收到 '%s'）"), *InMode);
			return false;
		}

		List->ScrollIndexIntoView(InIndex);
		OutPayload->SetNumberField(TEXT("index"), InIndex);
		OutPayload->SetBoolField(TEXT("refreshPending"), List->IsRefreshPending());
		OutMessage = FString::Printf(TEXT("已请求把第 %d 项滚入视野"), InIndex);
		return true;
	}

	OutMessage = FString::Printf(TEXT("%s（%s）不是可滚动的容器；支持 ScrollBox 与 ListView"),
		*InWidget->GetFName().ToString(), *InWidget->GetClass()->GetName());
	return false;
}

bool GameTestOps::SetValue(UWidget* InWidget, const FJsonObject& InParams, FString& OutMessage, TSharedPtr<FJsonObject>& OutPayload)
{
	if (InWidget == nullptr)
	{
		OutMessage = TEXT("目标控件已失效");
		return false;
	}

	double NumberValue = 0.0;
	bool bBooleanValue = false;
	int32 IntValue = 0;
	FString StringValue;

	if (USlider* Slider = Cast<USlider>(InWidget))
	{
		if (!RejectUnknownParams(InParams, { TEXT("value") }, OutMessage))
		{
			return false;
		}
		if (!GameTestJson::TryGetNumber(InParams, TEXT("value"), NumberValue))
		{
			OutMessage = TEXT("滑条需要 value（数值）");
			return false;
		}

		Slider->SetValue(static_cast<float>(NumberValue));
		OutPayload->SetNumberField(TEXT("value"), Slider->GetValue());
		OutMessage = FString::Printf(TEXT("滑条 → %.3f"), Slider->GetValue());
		return true;
	}

	if (USpinBox* SpinBox = Cast<USpinBox>(InWidget))
	{
		if (!RejectUnknownParams(InParams, { TEXT("value") }, OutMessage))
		{
			return false;
		}
		if (!GameTestJson::TryGetNumber(InParams, TEXT("value"), NumberValue))
		{
			OutMessage = TEXT("微调框需要 value（数值）");
			return false;
		}

		const float NewValue = static_cast<float>(NumberValue);
		// SetValue 只改值（实测：不广播 OnValueChanged），所以要补一次。
		SpinBox->SetValue(NewValue);
		SpinBox->OnValueChanged.Broadcast(NewValue);
		OutPayload->SetNumberField(TEXT("value"), NewValue);
		OutMessage = FString::Printf(TEXT("微调框 → %.3f（已广播 OnValueChanged）"), NewValue);
		return true;
	}

	if (UCheckBox* CheckBox = Cast<UCheckBox>(InWidget))
	{
		if (!RejectUnknownParams(InParams, { TEXT("checked") }, OutMessage))
		{
			return false;
		}
		if (!GameTestJson::TryGetBool(InParams, TEXT("checked"), bBooleanValue))
		{
			OutMessage = TEXT("复选需要 checked（true/false）");
			return false;
		}

		CheckBox->SetIsChecked(bBooleanValue);
		CheckBox->OnCheckStateChanged.Broadcast(bBooleanValue);
		OutPayload->SetBoolField(TEXT("checked"), CheckBox->IsChecked());
		OutMessage = FString::Printf(TEXT("复选 → %s（已广播 OnCheckStateChanged）"),
			CheckBox->IsChecked() ? TEXT("true") : TEXT("false"));
		return true;
	}

	if (UComboBoxString* ComboBox = Cast<UComboBoxString>(InWidget))
	{
		if (!RejectUnknownParams(InParams, { TEXT("option"), TEXT("index") }, OutMessage))
		{
			return false;
		}

		if (GameTestJson::TryGetInt(InParams, TEXT("index"), IntValue))
		{
			ComboBox->SetSelectedIndex(IntValue);
		}
		else if (GameTestJson::TryGetString(InParams, TEXT("option"), StringValue))
		{
			ComboBox->SetSelectedOption(StringValue);
		}
		else
		{
			OutMessage = TEXT("下拉框需要 option（选项文本）或 index（序号）");
			return false;
		}

		OutPayload->SetStringField(TEXT("option"), ComboBox->GetSelectedOption());
		OutPayload->SetNumberField(TEXT("index"), ComboBox->GetSelectedIndex());
		OutMessage = FString::Printf(TEXT("下拉框 → '%s'"), *ComboBox->GetSelectedOption());
		return true;
	}

	if (UWidgetSwitcher* Switcher = Cast<UWidgetSwitcher>(InWidget))
	{
		if (!RejectUnknownParams(InParams, { TEXT("index") }, OutMessage))
		{
			return false;
		}
		if (!GameTestJson::TryGetInt(InParams, TEXT("index"), IntValue))
		{
			OutMessage = TEXT("切换器需要 index（页号，从 0 开始）");
			return false;
		}

		return SetSwitcherIndex(Switcher, IntValue, OutMessage, OutPayload);
	}

	if (UExpandableArea* Area = Cast<UExpandableArea>(InWidget))
	{
		if (!RejectUnknownParams(InParams, { TEXT("expanded") }, OutMessage))
		{
			return false;
		}
		if (!GameTestJson::TryGetBool(InParams, TEXT("expanded"), bBooleanValue))
		{
			OutMessage = TEXT("展开器需要 expanded（true/false）");
			return false;
		}

		Area->SetIsExpanded(bBooleanValue);
		OutPayload->SetBoolField(TEXT("expanded"), Area->GetIsExpanded());
		// 引擎的 SetIsExpanded 只走字段通知，不广播 OnExpansionChanged；真实点击会广播，
		// 所以这里补一次，否则"代码级展开"与"人点一下"对游戏逻辑不等价。
		Area->OnExpansionChanged.Broadcast(Area, bBooleanValue);
		SetDelegateFiredFlag(OutPayload, TEXT("delegateFired"), true);
		OutMessage = FString::Printf(TEXT("展开器 → %s（已同步广播 OnExpansionChanged）"),
			Area->GetIsExpanded() ? TEXT("expanded") : TEXT("collapsed"));
		return true;
	}

	if (UMenuAnchor* MenuAnchor = Cast<UMenuAnchor>(InWidget))
	{
		if (!RejectUnknownParams(InParams, { TEXT("open") }, OutMessage))
		{
			return false;
		}
		if (!GameTestJson::TryGetBool(InParams, TEXT("open"), bBooleanValue))
		{
			OutMessage = TEXT("菜单锚需要 open（true/false）");
			return false;
		}

		if (bBooleanValue)
		{
			MenuAnchor->Open(/*bFocusMenu=*/false);
		}
		else
		{
			MenuAnchor->Close();
		}

		OutPayload->SetBoolField(TEXT("open"), MenuAnchor->IsOpen());
		OutMessage = FString::Printf(TEXT("菜单 → %s"), MenuAnchor->IsOpen() ? TEXT("open") : TEXT("closed"));
		return true;
	}

	if (UInputKeySelector* KeySelector = Cast<UInputKeySelector>(InWidget))
	{
		if (!RejectUnknownParams(InParams, { TEXT("key") }, OutMessage))
		{
			return false;
		}
		if (!GameTestJson::TryGetString(InParams, TEXT("key"), StringValue))
		{
			OutMessage = TEXT("按键选择器需要 key（按键名，如 Space / LeftMouseButton）");
			return false;
		}

		const FKey Key = FKey(FName(*StringValue));
		if (!Key.IsValid())
		{
			OutMessage = FString::Printf(TEXT("'%s' 不是合法按键名"), *StringValue);
			return false;
		}

		KeySelector->SetSelectedKey(FInputChord(Key));
		OutPayload->SetStringField(TEXT("key"), KeySelector->GetSelectedKey().Key.GetFName().ToString());
		OutMessage = FString::Printf(TEXT("按键 → %s"), *KeySelector->GetSelectedKey().Key.GetFName().ToString());
		return true;
	}

	OutMessage = FString::Printf(
		TEXT("%s（%s）没有可用的代码级赋值入口；set 认这些：滑条/微调框 value、复选 checked、下拉 option/index、")
		TEXT("切换器 index、展开器 expanded、TreeView expanded（配 index 或 property+equals）、菜单锚 open、按键选择器 key"),
		*InWidget->GetFName().ToString(), *InWidget->GetClass()->GetName());
	return false;
}

void GameTestOps::CollectLoadedWidgetClasses(TArray<FString>& OutClassPaths, int32 InLimit)
{
	for (TObjectIterator<UClass> Iterator; Iterator; ++Iterator)
	{
		UClass* Class = *Iterator;
		if (Class == nullptr || !Class->IsChildOf(UUserWidget::StaticClass()) || Class->HasAnyFlags(RF_ClassDefaultObject))
		{
			continue;
		}

		if (Class == UUserWidget::StaticClass())
		{
			continue;
		}

		OutClassPaths.Add(Class->GetPathName());
		if (OutClassPaths.Num() >= InLimit)
		{
			return;
		}
	}

	OutClassPaths.Sort();
}

UClass* GameTestOps::ResolveWidgetClass(const FString& InClassPath, UClass* InRequiredBase, FString& OutError)
{
	UClass* WidgetClass = nullptr;

	if (InClassPath.StartsWith(TEXT("/")))
	{
		FString FullPath = InClassPath;
		if (!FullPath.Contains(TEXT(".")))
		{
			FullPath = FString::Printf(TEXT("%s.%s_C"), *InClassPath, *FPaths::GetCleanFilename(InClassPath));
		}
		WidgetClass = StaticLoadClass(InRequiredBase, nullptr, *FullPath);
	}
	else
	{
		// 只给了资产名：在已加载的类里找（场景里通常先 snapshot 一下就能拿到类路径）。
		for (TObjectIterator<UClass> Iterator; Iterator; ++Iterator)
		{
			UClass* Class = *Iterator;
			if (Class != nullptr && Class->IsChildOf(InRequiredBase) && !Class->HasAnyFlags(RF_ClassDefaultObject))
			{
				const FString ClassName = Class->GetName();
				if (ClassName.Equals(InClassPath, ESearchCase::IgnoreCase)
					|| ClassName.Equals(InClassPath + TEXT("_C"), ESearchCase::IgnoreCase))
				{
					WidgetClass = Class;
					break;
				}
			}
		}
	}

	if (WidgetClass == nullptr)
	{
		TArray<FString> Candidates;
		CollectLoadedWidgetClasses(Candidates);
		OutError = FString::Printf(TEXT("找不到控件类 '%s'（要写 /Game/... 的类路径，且必须派生自 %s）；已加载的候选：%s"),
			*InClassPath, *InRequiredBase->GetName(),
			Candidates.Num() > 0 ? *FString::Join(Candidates, TEXT("、")) : TEXT("（一个都没有）"));
	}

	return WidgetClass;
}

bool GameTestOps::SetSwitcherIndex(UWidgetSwitcher* InSwitcher, int32 InIndex, FString& OutMessage, TSharedPtr<FJsonObject>& OutPayload)
{
	if (InSwitcher == nullptr)
	{
		OutMessage = TEXT("目标不是 WidgetSwitcher（页切换只能用在切换器上）");
		return false;
	}

	const int32 NumWidgets = InSwitcher->GetNumWidgets();
	if (InIndex < 0 || InIndex >= NumWidgets)
	{
		OutMessage = FString::Printf(TEXT("页号 %d 越界：这个切换器有 %d 页（0..%d）"), InIndex, NumWidgets, NumWidgets - 1);
		return false;
	}

	InSwitcher->SetActiveWidgetIndex(InIndex);
	const int32 Active = InSwitcher->GetActiveWidgetIndex();

	OutPayload->SetNumberField(TEXT("activeIndex"), Active);
	OutPayload->SetNumberField(TEXT("numWidgets"), NumWidgets);
	OutMessage = FString::Printf(TEXT("已切到第 %d 页（共 %d 页）"), Active, NumWidgets);
	return true;
}

bool GameTestOps::SetListItemExpansion(UTreeView* InTree, UObject* InItem, bool bInExpanded, FString& OutMessage, TSharedPtr<FJsonObject>& OutPayload)
{
	if (InTree == nullptr || InItem == nullptr)
	{
		OutMessage = TEXT("展开条目需要目标是一个 TreeView 且条目存在");
		return false;
	}

	InTree->SetItemExpansion(InItem, bInExpanded);

	// 展开态没有公开读接口（见头文件说明），能读回的是"已生成条目数"：折叠时只有根有控件，
	// 展开后子条目才会生成。它同时也是场景里可以断言的观测量（state.displayedEntries）。
	const int32 Displayed = InTree->GetDisplayedEntryWidgets().Num();

	OutPayload->SetBoolField(TEXT("expanded"), bInExpanded);
	OutPayload->SetNumberField(TEXT("displayedEntries"), Displayed);
	OutMessage = FString::Printf(TEXT("已%s条目「%s」（当前已生成 %d 个条目控件）"),
		bInExpanded ? TEXT("展开") : TEXT("收起"), *InItem->GetName(), Displayed);
	return true;
}

bool GameTestOps::AddWidgetToViewport(
	UWorld* InWorld,
	const FString& InClassPath,
	int32 InZOrder,
	UUserWidget*& OutWidget,
	FString& OutMessage)
{
	OutWidget = nullptr;

	if (InWorld == nullptr)
	{
		OutMessage = TEXT("没有正在运行的游戏世界（先 pie.start 或 wait until=pieReady）");
		return false;
	}

	UClass* WidgetClass = ResolveWidgetClass(InClassPath, UUserWidget::StaticClass(), OutMessage);
	if (WidgetClass == nullptr)
	{
		return false;
	}

	APlayerController* PlayerController = InWorld->GetFirstPlayerController();

	// CreateWidget 的模板版对宿主类型做静态断言（只接受 PlayerController / GameInstance / World 这些）；
	// 把宿主装进 UObject* 会让断言失败，所以两条分支各自用具体类型调。
	OutWidget = PlayerController != nullptr
		? CreateWidget<UUserWidget>(PlayerController, WidgetClass)
		: CreateWidget<UUserWidget>(InWorld, WidgetClass);
	if (OutWidget == nullptr)
	{
		OutMessage = FString::Printf(TEXT("CreateWidget 失败：%s"), *WidgetClass->GetName());
		return false;
	}

	OutWidget->AddToViewport(InZOrder);
	OutMessage = FString::Printf(TEXT("已创建并加进视口：%s（zOrder=%d）"), *WidgetClass->GetName(), InZOrder);
	return true;
}

bool GameTestOps::RemoveWidget(UUserWidget* InWidget, FString& OutMessage)
{
	if (InWidget == nullptr)
	{
		OutMessage = TEXT("目标控件已失效");
		return false;
	}

	const FString Name = InWidget->GetClass()->GetName();
	InWidget->RemoveFromParent();
	OutMessage = FString::Printf(TEXT("已从视口移除：%s"), *Name);
	return true;
}
