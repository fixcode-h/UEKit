// Copyright Epic Games, Inc. All Rights Reserved.

#include "Operate/GameTestCodeClick.h"
#include "GameTestCompat.h"

#include "Input/GameTestInput.h"
#include "Observe/GameTestSlate.h"
#include "Operate/GameTestOps.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/MenuAnchor.h"
#include "Dom/JsonObject.h"
#include "Widgets/SWidget.h"

namespace
{
	bool IsCodeClickable(const UWidget* InWidget)
	{
		return InWidget != nullptr
			&& (InWidget->IsA<UButton>() || InWidget->IsA<UCheckBox>() || InWidget->IsA<UMenuAnchor>());
	}

	/** 控件真在屏上（自己和整条 Slate 父链都可见）：WBP 里藏着的备用按钮不算。 */
	bool IsCodeClickVisible(const UWidget& InWidget)
	{
		const TSharedPtr<SWidget> Slate = InWidget.GetCachedWidget();
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
		return true;
	}

	/**
	 * 从目标往上找最近的可点控件，到所在 WBP 的外壳（SObjectWidget）为止：再往上就是别的界面的按钮了，
	 * 点它不是录制时点的东西。
	 */
	UWidget* FindClickableUp(const TSharedPtr<SWidget>& InTarget)
	{
		const TSharedPtr<SWidget> Viewport = GameTestSlate::GetGameViewportWidget();
		for (TSharedPtr<SWidget> Current = InTarget; Current.IsValid() && Current != Viewport; Current = Current->GetParentWidget())
		{
			UWidget* Widget = GameTestSlate::FindSourceWidget(*Current);
			if (IsCodeClickable(Widget))
			{
				return Widget;
			}
			if (Current != InTarget && Widget != nullptr && Widget->IsA<UUserWidget>())
			{
				return nullptr;
			}
		}
		return nullptr;
	}

	/** 目标是一个 WBP（列表条目、入口图标）：里面恰好一个可见的可点控件时就点它，有几个就说不清点哪个，不猜。 */
	UWidget* FindSingleClickableInside(const TSharedPtr<SWidget>& InTarget)
	{
		UUserWidget* UserWidget = Cast<UUserWidget>(GameTestSlate::FindSourceWidget(*InTarget));
		if (UserWidget == nullptr || UserWidget->WidgetTree == nullptr)
		{
			return nullptr;
		}
		UWidget* Found = nullptr;
		int32 Count = 0;
		UserWidget->WidgetTree->ForEachWidget([&Found, &Count](UWidget* InWidget)
		{
			if (IsCodeClickable(InWidget) && IsCodeClickVisible(*InWidget))
			{
				Found = InWidget;
				++Count;
			}
		});
		return Count == 1 ? Found : nullptr;
	}
}

bool GameTestCodeClick::Click(const TSharedPtr<SWidget>& InTarget, const FVector2D& InScreen, FJsonObject& OutPayload, FString& OutMessage)
{
	if (!InTarget.IsValid())
	{
		OutMessage = TEXT("目标控件已失效");
		return false;
	}

	UWidget* Clickable = FindClickableUp(InTarget);
	if (Clickable == nullptr)
	{
		Clickable = FindSingleClickableInside(InTarget);
	}

	if (Clickable != nullptr)
	{
		TSharedPtr<FJsonObject> Extra = MakeShared<FJsonObject>();
		if (!GameTestOps::Click(Clickable, OutMessage, Extra))
		{
			return false;
		}
		for (const auto& Pair : Extra->Values)
		{
			OutPayload.SetField(GAMETEST_JSON_KEY(Pair.Key), Pair.Value);
		}
		OutPayload.SetStringField(TEXT("via"), TEXT("code"));
		OutPayload.SetStringField(TEXT("clicked"),
			FString::Printf(TEXT("%s（%s）"), *Clickable->GetName(), *Clickable->GetClass()->GetName()));
		return true;
	}

	// 没有按钮这类控件可调：在控件位置发虚拟鼠标事件（不移系统光标），由控件自己的 OnMouseButtonDown / Up 处理
	if (!GameTestInput::SendVirtualClick(InScreen, OutMessage))
	{
		return false;
	}
	OutPayload.SetStringField(TEXT("via"), TEXT("virtualPointer"));
	OutPayload.SetStringField(TEXT("clicked"), GameTestSlate::Describe(InTarget));
	return true;
}
