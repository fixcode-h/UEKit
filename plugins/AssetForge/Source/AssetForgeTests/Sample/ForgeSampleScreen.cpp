// Copyright Epic Games, Inc. All Rights Reserved.

#include "Sample/ForgeSampleScreen.h"

#include "Sample/ForgeSampleLists.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/InputKeySelector.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/ProgressBar.h"
#include "Components/Slider.h"
#include "Components/SpinBox.h"
#include "Components/TextBlock.h"
#include "Components/WidgetSwitcher.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogForgeSample, Log, All);

// ---- 转发对象 ----

void UForgeSampleBinding::OnClicked()
{
	if (UForgeSampleScreen* Owner = Screen.Get())
	{
		Owner->HandleClick(Source.Get());
	}
}

void UForgeSampleBinding::OnCheckChanged(bool bIsChecked)
{
	if (UForgeSampleScreen* Owner = Screen.Get())
	{
		Owner->HandleCheck(Source.Get(), bIsChecked);
	}
}

void UForgeSampleBinding::OnValueChanged(float InValue)
{
	if (UForgeSampleScreen* Owner = Screen.Get())
	{
		Owner->HandleValue(Source.Get(), InValue);
	}
}

void UForgeSampleBinding::OnOptionChanged(FString InSelectedItem, ESelectInfo::Type InSelectionType)
{
	if (UForgeSampleScreen* Owner = Screen.Get())
	{
		Owner->HandleOption(Source.Get(), InSelectedItem);
	}
}

void UForgeSampleBinding::OnTextCommitted(const FText& InText, ETextCommit::Type InCommitMethod)
{
	if (UForgeSampleScreen* Owner = Screen.Get())
	{
		Owner->HandleCommit(Source.Get(), InText);
	}
}

void UForgeSampleBinding::OnPressed()
{
	if (UForgeSampleScreen* Owner = Screen.Get())
	{
		Owner->HandlePressed(Source.Get());
	}
}

void UForgeSampleBinding::OnReleased()
{
	if (UForgeSampleScreen* Owner = Screen.Get())
	{
		Owner->HandleReleased(Source.Get());
	}
}

void UForgeSampleBinding::OnHovered()
{
	if (UForgeSampleScreen* Owner = Screen.Get())
	{
		Owner->HandleHover(Source.Get(), true);
	}
}

void UForgeSampleBinding::OnUnhovered()
{
	if (UForgeSampleScreen* Owner = Screen.Get())
	{
		Owner->HandleHover(Source.Get(), false);
	}
}

void UForgeSampleBinding::OnKeySelected(FInputChord InSelectedKey)
{
	if (UForgeSampleScreen* Owner = Screen.Get())
	{
		Owner->HandleKeySelected(Source.Get(), InSelectedKey);
	}
}

void UForgeSampleBinding::Unbind()
{
	UWidget* Widget = Source.Get();
	if (UButton* Button = Cast<UButton>(Widget))
	{
		Button->OnClicked.RemoveAll(this);
		Button->OnPressed.RemoveAll(this);
		Button->OnReleased.RemoveAll(this);
		Button->OnHovered.RemoveAll(this);
		Button->OnUnhovered.RemoveAll(this);
	}
	else if (UCheckBox* Check = Cast<UCheckBox>(Widget))
	{
		Check->OnCheckStateChanged.RemoveAll(this);
	}
	else if (USlider* Slider = Cast<USlider>(Widget))
	{
		Slider->OnValueChanged.RemoveAll(this);
	}
	else if (USpinBox* Spin = Cast<USpinBox>(Widget))
	{
		Spin->OnValueChanged.RemoveAll(this);
	}
	else if (UComboBoxString* Combo = Cast<UComboBoxString>(Widget))
	{
		Combo->OnSelectionChanged.RemoveAll(this);
	}
	else if (UEditableTextBox* TextBox = Cast<UEditableTextBox>(Widget))
	{
		TextBox->OnTextCommitted.RemoveAll(this);
	}
	else if (UEditableText* Text = Cast<UEditableText>(Widget))
	{
		Text->OnTextCommitted.RemoveAll(this);
	}
	else if (UMultiLineEditableTextBox* MultiLine = Cast<UMultiLineEditableTextBox>(Widget))
	{
		MultiLine->OnTextCommitted.RemoveAll(this);
	}
	else if (UInputKeySelector* KeySelector = Cast<UInputKeySelector>(Widget))
	{
		KeySelector->OnKeySelected.RemoveAll(this);
	}
}

// ---- 界面 ----

void UForgeSampleScreen::NativeConstruct()
{
	Super::NativeConstruct();

	// 「列表条目」页的仓库、邮件、技能栏：数据和运行时格子在这里填（配置里只有空容器）
	ForgeSampleLists::Populate(*this);
	BindAll();
	ResetScreen();

	if (bShowCursor)
	{
		if (APlayerController* Player = GetOwningPlayer())
		{
			Player->SetShowMouseCursor(true);
			FInputModeGameAndUI Mode;
			Mode.SetHideCursorDuringCapture(false);
			Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
			Player->SetInputMode(Mode);
		}
	}
}

void UForgeSampleScreen::NativeDestruct()
{
	if (UWorld* World = GetWorld())
	{
		for (FTimerHandle& Timer : Timers)
		{
			World->GetTimerManager().ClearTimer(Timer);
		}
	}
	Timers.Reset();
	UnbindAll();
	Super::NativeDestruct();
}

void UForgeSampleScreen::BindAll()
{
	UnbindAll();
	if (WidgetTree == nullptr)
	{
		return;
	}

	// 每一页是子 WBP：连同嵌套的蓝图子界面一起走，页里的按钮也按名字约定接上
	ForgeSampleLists::ForEachDeep(*this, [this](UWidget* Widget)
	{
		// 名字在整个界面里不重名（gen.mjs 核对）；万一重了留先找到的那个（自己的树优先）
		if (!ByName.Contains(Widget->GetFName()))
		{
			ByName.Add(Widget->GetFName(), Widget);
		}

		auto MakeBinding = [this, Widget]()
		{
			UForgeSampleBinding* Binding = NewObject<UForgeSampleBinding>(this);
			Binding->Screen = this;
			Binding->Source = Widget;
			Bindings.Add(Binding);
			return Binding;
		};

		if (UButton* Button = Cast<UButton>(Widget))
		{
			UForgeSampleBinding* Binding = MakeBinding();
			Button->OnClicked.AddDynamic(Binding, &UForgeSampleBinding::OnClicked);
			const FString Base = BaseName(Button);
			if (Base.StartsWith(TEXT("Go_")))
			{
				NormalTints.Add(Button, Button->GetBackgroundColor());
			}
			else if (Base.StartsWith(TEXT("Hold_")))
			{
				Button->OnPressed.AddDynamic(Binding, &UForgeSampleBinding::OnPressed);
				Button->OnReleased.AddDynamic(Binding, &UForgeSampleBinding::OnReleased);
			}
			else if (Base.StartsWith(TEXT("Hover_")))
			{
				Button->OnHovered.AddDynamic(Binding, &UForgeSampleBinding::OnHovered);
				Button->OnUnhovered.AddDynamic(Binding, &UForgeSampleBinding::OnUnhovered);
			}
		}
		else if (UCheckBox* Check = Cast<UCheckBox>(Widget))
		{
			Check->OnCheckStateChanged.AddDynamic(MakeBinding(), &UForgeSampleBinding::OnCheckChanged);
		}
		else if (USlider* Slider = Cast<USlider>(Widget))
		{
			Slider->OnValueChanged.AddDynamic(MakeBinding(), &UForgeSampleBinding::OnValueChanged);
		}
		else if (USpinBox* Spin = Cast<USpinBox>(Widget))
		{
			Spin->OnValueChanged.AddDynamic(MakeBinding(), &UForgeSampleBinding::OnValueChanged);
		}
		else if (UComboBoxString* Combo = Cast<UComboBoxString>(Widget))
		{
			Combo->OnSelectionChanged.AddDynamic(MakeBinding(), &UForgeSampleBinding::OnOptionChanged);
		}
		else if (UEditableTextBox* TextBox = Cast<UEditableTextBox>(Widget))
		{
			TextBox->OnTextCommitted.AddDynamic(MakeBinding(), &UForgeSampleBinding::OnTextCommitted);
		}
		else if (UEditableText* Text = Cast<UEditableText>(Widget))
		{
			Text->OnTextCommitted.AddDynamic(MakeBinding(), &UForgeSampleBinding::OnTextCommitted);
		}
		else if (UMultiLineEditableTextBox* MultiLine = Cast<UMultiLineEditableTextBox>(Widget))
		{
			MultiLine->OnTextCommitted.AddDynamic(MakeBinding(), &UForgeSampleBinding::OnTextCommitted);
		}
		else if (UInputKeySelector* KeySelector = Cast<UInputKeySelector>(Widget))
		{
			KeySelector->OnKeySelected.AddDynamic(MakeBinding(), &UForgeSampleBinding::OnKeySelected);
		}
	});
}

void UForgeSampleScreen::UnbindAll()
{
	for (UForgeSampleBinding* Binding : Bindings)
	{
		if (Binding != nullptr)
		{
			Binding->Unbind();
		}
	}
	Bindings.Reset();
	NormalTints.Reset();
	ByName.Reset();
}

void UForgeSampleScreen::ResetScreen()
{
	ResetFlow();
	History.Reset();
	CloseAllPopups();
	if (UWidgetSwitcher* Pages = PageSwitcher())
	{
		Pages->SetActiveWidgetIndex(0);
	}
	Refresh();
	SetStatus(TEXT("就绪"));
}

UWidget* UForgeSampleScreen::FindAny(const FString& InName) const
{
	if (const TWeakObjectPtr<UWidget>* Found = ByName.Find(FName(*InName)))
	{
		if (UWidget* Widget = Found->Get())
		{
			return Widget;
		}
	}
	// 还没 BindAll（构造早期）时退回到逐棵树找
	return WidgetTree != nullptr ? ForgeSampleLists::FindDeep(*const_cast<UForgeSampleScreen*>(this), FName(*InName)) : nullptr;
}

FString UForgeSampleScreen::BaseName(const UWidget* InWidget)
{
	if (InWidget == nullptr)
	{
		return FString();
	}
	FString Name = InWidget->GetName();
	const int32 Suffix = Name.Find(TEXT("__"), ESearchCase::CaseSensitive);
	if (Suffix != INDEX_NONE)
	{
		Name.LeftInline(Suffix);
	}
	return Name;
}

namespace
{
	/** 弹窗本身叫 `Popup_<一个词>`；它里面的 `Popup_Buy_Title`、`Popup_Buy_Subject` 不算 */
	bool IsPopupName(const FString& InName)
	{
		return InName.StartsWith(TEXT("Popup_")) && !InName.RightChop(6).Contains(TEXT("_"));
	}

	/**
	 * 按钮的提示文字。走反射读 `ToolTipText`：这个属性在不同引擎版本里的 C++ 访问方式不一样
	 * （直接成员 / 访问器），反射名字一直没变，5.4 和 5.8 都不用分支。
	 */
	FText ToolTipOf(const UWidget* InWidget)
	{
		static const FTextProperty* Property = FindFProperty<FTextProperty>(UWidget::StaticClass(), TEXT("ToolTipText"));
		return (InWidget != nullptr && Property != nullptr) ? Property->GetPropertyValue_InContainer(InWidget) : FText::GetEmpty();
	}
}

FString UForgeSampleScreen::LabelOf(const UWidget* InWidget) const
{
	if (InWidget == nullptr || WidgetTree == nullptr)
	{
		return FString();
	}
	if (const UTextBlock* Named = Cast<UTextBlock>(FindAny(TEXT("L_") + InWidget->GetName())))
	{
		return Named->GetText().ToString();
	}

	FString Found;
	if (const UPanelWidget* Panel = Cast<UPanelWidget>(InWidget))
	{
		UWidgetTree::ForWidgetAndChildren(const_cast<UPanelWidget*>(Panel), [&Found](UWidget* Child)
		{
			if (Found.IsEmpty())
			{
				if (const UTextBlock* Text = Cast<UTextBlock>(Child))
				{
					Found = Text->GetText().ToString();
				}
			}
		});
	}
	return Found.IsEmpty() ? InWidget->GetName() : Found;
}

FString UForgeSampleScreen::TitleOf(const FString& InName) const
{
	if (WidgetTree != nullptr)
	{
		if (const UTextBlock* Title = Cast<UTextBlock>(FindAny(InName + TEXT("_Title"))))
		{
			return Title->GetText().ToString();
		}
		if (const UWidget* Nav = FindAny(TEXT("Go_") + InName + TEXT("__Nav")))
		{
			return LabelOf(Nav);
		}
	}
	return InName;
}

UWidgetSwitcher* UForgeSampleScreen::PageSwitcher() const
{
	return WidgetTree != nullptr ? Cast<UWidgetSwitcher>(FindAny(PageSwitcherName.ToString())) : nullptr;
}

void UForgeSampleScreen::SetText(const FString& InWidgetName, const FString& InText)
{
	if (WidgetTree == nullptr)
	{
		return;
	}
	if (UTextBlock* Text = Cast<UTextBlock>(FindAny(InWidgetName)))
	{
		Text->SetText(FText::FromString(InText));
	}
}

void UForgeSampleScreen::SetStatus(const FString& InText)
{
	SetText(TEXT("Status"), InText);
	UE_LOG(LogForgeSample, Log, TEXT("%s"), *InText);
}

void UForgeSampleScreen::SetSubject(const FString& InTargetName, const UWidget* InSource)
{
	if (InSource == nullptr)
	{
		return;
	}
	const FText Tip = ToolTipOf(InSource);
	if (!Tip.IsEmpty())
	{
		SetText(InTargetName + TEXT("_Subject"), Tip.ToString());
	}
}

void UForgeSampleScreen::CloseAllPopups()
{
	OpenPopups.Reset();
	if (WidgetTree == nullptr)
	{
		return;
	}
	ForgeSampleLists::ForEachDeep(*this, [](UWidget* Widget)
	{
		if (IsPopupName(Widget->GetName()))
		{
			Widget->SetVisibility(ESlateVisibility::Collapsed);
		}
	});
}

void UForgeSampleScreen::GoTo(UWidget* InTarget, bool bRecordHistory)
{
	UWidgetSwitcher* Switcher = InTarget != nullptr ? Cast<UWidgetSwitcher>(InTarget->GetParent()) : nullptr;
	if (Switcher == nullptr)
	{
		return;
	}
	const bool bPages = Switcher == PageSwitcher();
	UWidget* Current = Switcher->GetActiveWidget();
	if (bPages)
	{
		CloseAllPopups();
		if (bRecordHistory && Current != nullptr && Current != InTarget)
		{
			History.Add(Current);
		}
	}
	Switcher->SetActiveWidget(InTarget);
	Refresh();
}

void UForgeSampleScreen::Back()
{
	// 先关最后打开、还开着的弹窗
	while (OpenPopups.Num() > 0)
	{
		UWidget* Popup = OpenPopups.Pop().Get();
		if (Popup != nullptr && Popup->IsVisible())
		{
			Popup->SetVisibility(ESlateVisibility::Collapsed);
			SetStatus(FString::Printf(TEXT("返回：关闭「%s」"), *TitleOf(Popup->GetName())));
			return;
		}
	}

	while (History.Num() > 0)
	{
		UWidget* Previous = History.Pop().Get();
		if (Previous != nullptr)
		{
			GoTo(Previous, /*bRecordHistory=*/false);
			SetStatus(FString::Printf(TEXT("返回：%s"), *TitleOf(Previous->GetName())));
			return;
		}
	}
	SetStatus(TEXT("返回：已经是第一页"));
}

void UForgeSampleScreen::Refresh()
{
	UWidgetSwitcher* Pages = PageSwitcher();
	if (Pages != nullptr && Pages->GetActiveWidget() != nullptr)
	{
		SetText(TEXT("Crumb"), TitleOf(Pages->GetActiveWidget()->GetName()));
	}

	for (const TPair<TWeakObjectPtr<UButton>, FLinearColor>& Pair : NormalTints)
	{
		UButton* Button = Pair.Key.Get();
		if (Button == nullptr || WidgetTree == nullptr)
		{
			continue;
		}
		const UWidget* Target = FindAny(BaseName(Button).RightChop(3));
		const UWidgetSwitcher* Switcher = Target != nullptr ? Cast<UWidgetSwitcher>(Target->GetParent()) : nullptr;
		const bool bActive = Switcher != nullptr && Switcher->GetActiveWidget() == Target;
		Button->SetBackgroundColor(bActive ? ActiveTint : Pair.Value);
	}
}

void UForgeSampleScreen::HandleClick(UWidget* InSource)
{
	if (InSource == nullptr || WidgetTree == nullptr)
	{
		return;
	}
	const FString Name = BaseName(InSource);
	const FString Label = LabelOf(InSource);

	if (Name == TEXT("Back"))
	{
		Back();
		return;
	}
	// 长按、悬停按钮的结果由按下 / 松开、进出事件写，点击本身不再覆盖状态栏
	if (Name.StartsWith(TEXT("Hold_")) || Name.StartsWith(TEXT("Hover_")))
	{
		return;
	}

	auto FindTarget = [this, &Name](int32 InPrefixLen) -> UWidget*
	{
		UWidget* Target = FindAny(Name.RightChop(InPrefixLen));
		if (Target == nullptr)
		{
			UE_LOG(LogForgeSample, Warning, TEXT("按钮 %s 指向的控件 %s 不存在"), *Name, *Name.RightChop(InPrefixLen));
		}
		return Target;
	};

	if (Name.StartsWith(TEXT("Go_")))
	{
		if (UWidget* Target = FindTarget(3))
		{
			SetSubject(Target->GetName(), InSource);
			GoTo(Target, /*bRecordHistory=*/true);
			SetStatus(FString::Printf(TEXT("%s：进入「%s」"), *Label, *TitleOf(Target->GetName())));
		}
	}
	else if (Name.StartsWith(TEXT("Open_")))
	{
		if (UWidget* Target = FindTarget(5))
		{
			OpenPopup(Target, InSource, Label);
		}
	}
	else if (Name.StartsWith(TEXT("Close_")))
	{
		if (UWidget* Target = FindTarget(6))
		{
			Target->SetVisibility(ESlateVisibility::Collapsed);
			OpenPopups.Remove(Target);
			SetStatus(FString::Printf(TEXT("%s：关闭「%s」"), *Label, *TitleOf(Target->GetName())));
		}
	}
	else if (Name.StartsWith(TEXT("Pick_")))
	{
		SetText(Name.RightChop(5), Label);
		SetSubject(Name.RightChop(5), InSource);
		SetStatus(FString::Printf(TEXT("选中「%s」"), *Label));
	}
	else if (Name.StartsWith(TEXT("Submit_")))
	{
		const FString InputName = Name.RightChop(7);
		UWidget* Input = FindTarget(7);
		FString Value;
		if (const UEditableTextBox* TextBox = Cast<UEditableTextBox>(Input))
		{
			Value = TextBox->GetText().ToString();
		}
		else if (const UEditableText* Text = Cast<UEditableText>(Input))
		{
			Value = Text->GetText().ToString();
		}
		else if (const UMultiLineEditableTextBox* MultiLine = Cast<UMultiLineEditableTextBox>(Input))
		{
			Value = MultiLine->GetText().ToString();
		}
		SetText(InputName + TEXT("_Echo"), Value.IsEmpty() ? TEXT("（空）") : Value);
		SetStatus(FString::Printf(TEXT("%s：已保存「%s」"), *Label, *Value));
	}
	else if (!HandleFlowClick(InSource, Name, Label))
	{
		SetStatus(FString::Printf(TEXT("点击「%s」"), *Label));
	}
}

void UForgeSampleScreen::OpenPopup(UWidget* InPopup, const UWidget* InSource, const FString& InLabel)
{
	if (InPopup == nullptr)
	{
		return;
	}
	SetSubject(InPopup->GetName(), InSource);
	InPopup->SetVisibility(ESlateVisibility::Visible);
	OpenPopups.Remove(InPopup);
	OpenPopups.Add(InPopup);
	SetStatus(FString::Printf(TEXT("%s：打开「%s」"), *InLabel, *TitleOf(InPopup->GetName())));
}

void UForgeSampleScreen::HandleCheck(UWidget* InSource, bool bIsChecked)
{
	if (InSource == nullptr)
	{
		return;
	}
	const FString State = bIsChecked ? TEXT("开") : TEXT("关");
	SetText(InSource->GetName() + TEXT("_Value"), State);
	const FString Name = BaseName(InSource);
	if (Name.StartsWith(TEXT("Enable_")))
	{
		if (UWidget* Target = FindAny(Name.RightChop(7)))
		{
			Target->SetIsEnabled(bIsChecked);
		}
	}
	SetStatus(FString::Printf(TEXT("%s：%s"), *LabelOf(InSource), *State));
}

void UForgeSampleScreen::HandleValue(UWidget* InSource, float InValue)
{
	if (InSource == nullptr)
	{
		return;
	}
	const FString Value = FString::FromInt(FMath::RoundToInt(InValue));
	SetText(InSource->GetName() + TEXT("_Value"), Value);
	if (const USlider* Slider = Cast<USlider>(InSource))
	{
		if (UProgressBar* Bar = Cast<UProgressBar>(FindAny(InSource->GetName() + TEXT("_Bar"))))
		{
			const float Range = Slider->GetMaxValue() - Slider->GetMinValue();
			Bar->SetPercent(Range > 0.0f ? (InValue - Slider->GetMinValue()) / Range : 0.0f);
		}
	}
	SetStatus(FString::Printf(TEXT("%s = %s"), *TitleOf(InSource->GetName()), *Value));
}

void UForgeSampleScreen::HandleOption(UWidget* InSource, const FString& InItem)
{
	if (InSource == nullptr)
	{
		return;
	}
	SetText(InSource->GetName() + TEXT("_Value"), InItem);
	SetStatus(FString::Printf(TEXT("%s：%s"), *TitleOf(InSource->GetName()), *InItem));
}

void UForgeSampleScreen::HandleCommit(UWidget* InSource, const FText& InText)
{
	if (InSource == nullptr)
	{
		return;
	}
	SetStatus(FString::Printf(TEXT("%s 输入：%s"), *TitleOf(InSource->GetName()), *InText.ToString()));
}
