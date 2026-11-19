// Copyright Epic Games, Inc. All Rights Reserved.

// 示例界面「流程」「控件」两页的名字约定（计数、领取、延时显示、定时弹窗、长按、悬停、菜单）。
// 和 ForgeSampleScreen.cpp 分开放：那边是跳页、弹窗这些基础约定，这边只给 GameTest 的脚本功能当靶子。

#include "Sample/ForgeSampleScreen.h"

#include "Sample/ForgeSampleLists.h"

#include "Components/Button.h"
#include "Components/MenuAnchor.h"
#include "Components/TextBlock.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "TimerManager.h"

namespace ForgeSampleFlowDetail
{
	/** `Delay_` 显示出来的控件（ResetFlow 时藏回去）都以这个开头 */
	const TCHAR* const RevealPrefix = TEXT("Reveal_");

	int32 ReadInt(const UTextBlock* InText)
	{
		return InText != nullptr ? FCString::Atoi(*InText->GetText().ToString()) : 0;
	}
}

void UForgeSampleScreen::SetVisible(UWidget* InWidget, bool bInVisible)
{
	if (InWidget != nullptr)
	{
		InWidget->SetVisibility(bInVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
}

bool UForgeSampleScreen::HandleFlowClick(UWidget* InSource, const FString& InName, const FString& InLabel)
{
	using namespace ForgeSampleFlowDetail;

	// 计数：Inc_ / Dec_ / Reset_ + 文字控件名
	const bool bInc = InName.StartsWith(TEXT("Inc_"));
	const bool bDec = InName.StartsWith(TEXT("Dec_"));
	const bool bReset = InName.StartsWith(TEXT("Reset_"));
	if (bInc || bDec || bReset)
	{
		const FString TextName = InName.RightChop(bReset ? 6 : 4);
		UTextBlock* Text = Cast<UTextBlock>(FindAny(TextName));
		if (Text == nullptr)
		{
			return false;
		}
		const int32 Value = bReset ? 0 : ReadInt(Text) + (bInc ? 1 : -1);
		if (TextName == TEXT("Counter"))
		{
			SetCounter(Value);
		}
		else
		{
			Text->SetText(FText::FromString(FString::FromInt(Value)));
		}
		SetStatus(FString::Printf(TEXT("%s：%s = %d"), *InLabel, *TitleOf(TextName), Value));
		return true;
	}

	if (InName.StartsWith(TEXT("Claim_")))
	{
		const FString What = InName.RightChop(6);
		if (ClaimedNames.Contains(What))
		{
			SetStatus(FString::Printf(TEXT("「%s」已经领过了"), *What));
			return true;
		}
		ClaimedNames.Add(What);
		ClaimCount = ClaimedNames.Num();
		if (InSource != nullptr)
		{
			if (UTextBlock* Label = Cast<UTextBlock>(FindAny(TEXT("L_") + InSource->GetName())))
			{
				Label->SetText(FText::FromString(TEXT("已领取")));
			}
		}
		SetStatus(FString::Printf(TEXT("领取「%s」（共领了 %d 个）"), *What, ClaimCount));
		return true;
	}

	// Menu_<菜单锚点>：开 / 关菜单（菜单锚点自己不接点击，项目里也都是按钮去开它）
	if (InName.StartsWith(TEXT("Menu_")))
	{
		UMenuAnchor* Anchor = Cast<UMenuAnchor>(FindAny(InName.RightChop(5)));
		if (Anchor == nullptr)
		{
			return false;
		}
		Anchor->ToggleOpen(/*bFocusOnOpen=*/false);
		SetStatus(FString::Printf(TEXT("%s：%s菜单"), *InLabel, Anchor->IsOpen() ? TEXT("打开") : TEXT("关上")));
		return true;
	}

	if (InName.StartsWith(TEXT("Toggle_")))
	{
		UWidget* Target = FindAny(InName.RightChop(7));
		if (Target == nullptr)
		{
			return false;
		}
		const bool bShow = !Target->IsVisible();
		SetVisible(Target, bShow);
		SetStatus(FString::Printf(TEXT("%s：%s「%s」"), *InLabel, bShow ? TEXT("显示") : TEXT("隐藏"), *TitleOf(Target->GetName())));
		return true;
	}

	// Delay_ / Arm_：过一会儿才出现，GameTest 要等它（或者被守护处理掉）
	const bool bDelay = InName.StartsWith(TEXT("Delay_"));
	const bool bArm = InName.StartsWith(TEXT("Arm_"));
	if (bDelay || bArm)
	{
		UWidget* Target = FindAny(InName.RightChop(bDelay ? 6 : 4));
		UWorld* World = GetWorld();
		if (Target == nullptr || World == nullptr)
		{
			return false;
		}
		const float Seconds = bDelay ? RevealDelaySeconds : ArmDelaySeconds;
		TWeakObjectPtr<UWidget> WeakTarget = Target;
		TWeakObjectPtr<const UWidget> WeakSource = InSource;
		const FString Label = InLabel;
		FTimerHandle Handle;
		World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateWeakLambda(this, [this, WeakTarget, WeakSource, Label, bDelay]()
		{
			UWidget* Shown = WeakTarget.Get();
			if (Shown == nullptr)
			{
				return;
			}
			if (bDelay)
			{
				SetVisible(Shown, true);
				SetStatus(FString::Printf(TEXT("「%s」出来了"), *TitleOf(Shown->GetName())));
			}
			else
			{
				OpenPopup(Shown, WeakSource.Get(), Label);
			}
		}), FMath::Max(Seconds, 0.01f), /*bLoop=*/false);
		Timers.Add(Handle);
		SetStatus(FString::Printf(TEXT("%s：%.1f 秒后%s「%s」"), *InLabel, Seconds, bDelay ? TEXT("显示") : TEXT("弹出"), *TitleOf(Target->GetName())));
		return true;
	}
	return false;
}

void UForgeSampleScreen::SetCounter(int32 InValue)
{
	Counter = InValue;
	SetText(TEXT("Counter"), FString::FromInt(Counter));
}

int32 UForgeSampleScreen::AddCounter(int32 InDelta)
{
	SetCounter(Counter + InDelta);
	SetStatus(FString::Printf(TEXT("计数 %s%d = %d"), InDelta >= 0 ? TEXT("+") : TEXT(""), InDelta, Counter));
	return Counter;
}

void UForgeSampleScreen::ResetFlow()
{
	using namespace ForgeSampleFlowDetail;

	if (UWorld* World = GetWorld())
	{
		for (FTimerHandle& Timer : Timers)
		{
			World->GetTimerManager().ClearTimer(Timer);
		}
	}
	Timers.Reset();
	SetCounter(0);
	ClaimedNames.Reset();
	ClaimCount = 0;
	PressedAt.Reset();

	if (WidgetTree == nullptr)
	{
		return;
	}
	ForgeSampleLists::ForEachDeep(*this, [this](UWidget* Widget)
	{
		const FString Name = Widget->GetName();
		if (Name.StartsWith(RevealPrefix))
		{
			// 延时显示的面板、Toggle 切的面板配置里都是折叠的：藏回去
			Widget->SetVisibility(ESlateVisibility::Collapsed);
		}
		else if (Name.StartsWith(TEXT("L_Claim_")))
		{
			// 领取按钮的字恢复成配置里的（gen.mjs 里领取按钮一律写「领取」）
			if (UTextBlock* Label = Cast<UTextBlock>(Widget))
			{
				Label->SetText(FText::FromString(TEXT("领取")));
			}
		}
	});
}

void UForgeSampleScreen::CloseMenus()
{
	if (WidgetTree == nullptr)
	{
		return;
	}
	ForgeSampleLists::ForEachDeep(*this, [](UWidget* Widget)
	{
		if (UMenuAnchor* Anchor = Cast<UMenuAnchor>(Widget))
		{
			if (Anchor->IsOpen())
			{
				Anchor->Close();
			}
		}
	});
}

void UForgeSampleScreen::HandlePressed(UWidget* InSource)
{
	if (InSource != nullptr)
	{
		PressedAt.Add(InSource->GetFName(), FPlatformTime::Seconds());
		SetText(InSource->GetName() + TEXT("_Value"), TEXT("按住中…"));
	}
}

void UForgeSampleScreen::HandleReleased(UWidget* InSource)
{
	if (InSource == nullptr)
	{
		return;
	}
	double Started = 0.0;
	if (!PressedAt.RemoveAndCopyValue(InSource->GetFName(), Started))
	{
		return;
	}
	const double Held = FPlatformTime::Seconds() - Started;
	const bool bLong = Held >= HoldSeconds;
	SetText(InSource->GetName() + TEXT("_Value"), bLong ? TEXT("蓄力完成") : TEXT("短按"));
	SetStatus(FString::Printf(TEXT("%s：按住 %.1f 秒（%s）"), *LabelOf(InSource), Held, bLong ? TEXT("长按") : TEXT("短按")));
}

void UForgeSampleScreen::HandleHover(UWidget* InSource, bool bInHovered)
{
	if (InSource != nullptr)
	{
		SetText(InSource->GetName() + TEXT("_Value"), bInHovered ? TEXT("悬停中") : TEXT("未悬停"));
		if (bInHovered)
		{
			SetStatus(FString::Printf(TEXT("鼠标移到「%s」上"), *LabelOf(InSource)));
		}
	}
}

void UForgeSampleScreen::HandleKeySelected(UWidget* InSource, const FInputChord& InKey)
{
	if (InSource != nullptr)
	{
		const FString Key = InKey.Key.GetFName().ToString();
		SetText(InSource->GetName() + TEXT("_Value"), Key);
		SetStatus(FString::Printf(TEXT("%s：%s"), *TitleOf(InSource->GetName()), *Key));
	}
}
