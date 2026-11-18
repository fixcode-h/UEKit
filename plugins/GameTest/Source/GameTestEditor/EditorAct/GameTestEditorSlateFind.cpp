// Copyright Epic Games, Inc. All Rights Reserved.

#include "EditorAct/GameTestEditorSlateFind.h"

#include "Core/GameTestJson.h"
#include "Observe/GameTestSlate.h"

#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/Children.h"
#include "Layout/WidgetPath.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

namespace GameTestEditorSlateFindDetail
{
	/** 一次查找最多看多少个控件：编辑器开着十几个资源编辑器时控件树有十几万个节点，再多就是写法太宽。 */
	constexpr int32 MaxVisited = 200000;

	/** 能点的控件类型：点到按钮里的图标、文字时往上找到它们。 */
	bool IsClickableType(const FName InType)
	{
		static const TSet<FName> Types = {
			FName(TEXT("SButton")),
			FName(TEXT("SCheckBox")),
			FName(TEXT("SComboButton")),
			FName(TEXT("SHyperlink")),
			FName(TEXT("SSegmentedControl")),
		};
		return Types.Contains(InType);
	}

	/** 这些区域里的操作有对应的编辑器事件（选中、改属性、放 Actor），由事件录。 */
	bool IsSemanticType(const FName InType)
	{
		static const TSet<FName> Types = {
			FName(TEXT("SLevelViewport")),
			FName(TEXT("SEditorViewport")),
			FName(TEXT("SDetailsView")),
			FName(TEXT("SDetailsViewBase")),
			FName(TEXT("SSceneOutliner")),
			FName(TEXT("SAssetView")),
			FName(TEXT("SPathView")),
			FName(TEXT("SGraphEditor")),
		};
		return Types.Contains(InType);
	}

	FString TextOf(const TSharedRef<SWidget>& InWidget)
	{
		return GameTestSlate::CollectText(InWidget, 80).TrimStartAndEnd();
	}

	/** 停靠页栈里当前在前面的那一页的标签（栈的页签条在栈的上几层，深度有限）。 */
	FString ForegroundTabLabel(const TSharedRef<SWidget>& InStack, int32 InDepth)
	{
		if (InDepth > 6)
		{
			return FString();
		}
		FChildren* Children = InStack->GetChildren();
		if (Children == nullptr)
		{
			return FString();
		}
		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			const TSharedRef<SWidget> Child = Children->GetChildAt(Index);
			if (Child->GetType() == FName(TEXT("SDockTab")))
			{
				const TSharedRef<SDockTab> Tab = StaticCastSharedRef<SDockTab>(Child);
				if (Tab->IsForeground())
				{
					return Tab->GetTabLabel().ToString();
				}
				continue;
			}
			// 页内容也挂在栈下面，不往停靠页栈里套的栈走（那是里层的页）
			if (Child->GetType() == FName(TEXT("SDockingTabStack")))
			{
				continue;
			}
			const FString Found = ForegroundTabLabel(Child, InDepth + 1);
			if (!Found.IsEmpty())
			{
				return Found;
			}
		}
		return FString();
	}

	struct FFindState
	{
		const FGameTestEditorWidgetSpec* Spec = nullptr;
		FName Type;
		TArray<TSharedRef<SWidget>>* Out = nullptr;
		int32 Visited = 0;
	};

	void Visit(const TSharedRef<SWidget>& InWidget, FFindState& InOutState)
	{
		if (++InOutState.Visited > MaxVisited || !InWidget->GetVisibility().IsVisible())
		{
			return;
		}
		if (InWidget->GetType() == InOutState.Type)
		{
			const FGameTestEditorWidgetSpec& Spec = *InOutState.Spec;
			// 先比字再比停靠页：停靠页要往上走一遍父控件，只给字对上的算
			const bool bMatch = (Spec.Text.IsEmpty() || TextOf(InWidget).Equals(Spec.Text, ESearchCase::IgnoreCase))
				&& (Spec.Tab.IsEmpty() || GameTestEditorSlateFind::TabChain(InWidget) == Spec.Tab);
			if (bMatch && !GameTestSlate::IsInGameUi(InWidget))
			{
				InOutState.Out->Add(InWidget);
			}
		}
		FChildren* Children = InWidget->GetChildren();
		if (Children == nullptr)
		{
			return;
		}
		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			Visit(Children->GetChildAt(Index), InOutState);
		}
	}

	void CollectWindows(const TSharedRef<SWindow>& InWindow, TArray<TSharedRef<SWindow>>& OutWindows)
	{
		OutWindows.Add(InWindow);
		for (const TSharedRef<SWindow>& Child : InWindow->GetChildWindows())
		{
			CollectWindows(Child, OutWindows);
		}
	}
}

void FGameTestEditorWidgetSpec::WriteTo(FJsonObject& OutJson) const
{
	if (!Tab.IsEmpty())
	{
		OutJson.SetStringField(TEXT("tab"), Tab);
	}
	else if (!Window.IsEmpty())
	{
		OutJson.SetStringField(TEXT("window"), Window);
	}
	OutJson.SetStringField(TEXT("widget"), Widget);
	if (!Text.IsEmpty())
	{
		OutJson.SetStringField(TEXT("text"), Text);
	}
	if (Nth > 0)
	{
		OutJson.SetNumberField(TEXT("nth"), Nth);
	}
}

bool FGameTestEditorWidgetSpec::ReadFrom(const FJsonObject& InJson, FGameTestEditorWidgetSpec& OutSpec, FString& OutError)
{
	GameTestJson::TryGetString(InJson, TEXT("tab"), OutSpec.Tab);
	GameTestJson::TryGetString(InJson, TEXT("window"), OutSpec.Window);
	GameTestJson::TryGetString(InJson, TEXT("text"), OutSpec.Text);
	GameTestJson::TryGetInt(InJson, TEXT("nth"), OutSpec.Nth);
	if (!GameTestJson::TryGetString(InJson, TEXT("widget"), OutSpec.Widget) || OutSpec.Widget.IsEmpty())
	{
		OutError = TEXT("要写 widget（Slate 类型名，例如 SButton），录制时会自动填");
		return false;
	}
	if (OutSpec.Nth < 0)
	{
		OutError = TEXT("nth 从 0 数，不能是负数");
		return false;
	}
	return true;
}

FString FGameTestEditorWidgetSpec::Describe() const
{
	const FString Where = !Tab.IsEmpty() ? Tab : (!Window.IsEmpty() ? FString::Printf(TEXT("窗口「%s」"), *Window) : TEXT("编辑器"));
	FString Text2 = Text.IsEmpty() ? Widget : FString::Printf(TEXT("%s「%s」"), *Widget, *Text);
	if (Nth > 0)
	{
		Text2 += FString::Printf(TEXT(" 第 %d 个"), Nth + 1);
	}
	return FString::Printf(TEXT("%s · %s"), *Where, *Text2);
}

TSharedPtr<SWidget> GameTestEditorSlateFind::FindClickable(const FWidgetPath& InPath)
{
	if (!InPath.IsValid())
	{
		return nullptr;
	}
	const int32 Last = InPath.Widgets.Num() - 1;
	for (int32 Index = Last; Index >= 0 && Index >= Last - 10; --Index)
	{
		const TSharedRef<SWidget>& Widget = InPath.Widgets[Index].Widget;
		if (GameTestEditorSlateFindDetail::IsClickableType(Widget->GetType()))
		{
			return Widget;
		}
	}
	return InPath.GetLastWidget();
}

bool GameTestEditorSlateFind::IsSemanticArea(const FWidgetPath& InPath)
{
	for (int32 Index = 0; Index < InPath.Widgets.Num(); ++Index)
	{
		if (GameTestEditorSlateFindDetail::IsSemanticType(InPath.Widgets[Index].Widget->GetType()))
		{
			return true;
		}
	}
	return false;
}

FString GameTestEditorSlateFind::TabChain(const TSharedRef<SWidget>& InWidget)
{
	TArray<FString> Labels;
	TSharedPtr<SWidget> Cursor = InWidget->GetParentWidget();
	for (int32 Guard = 0; Cursor.IsValid() && Guard < 400; ++Guard)
	{
		if (Cursor->GetType() == FName(TEXT("SDockingTabStack")))
		{
			const FString Label = GameTestEditorSlateFindDetail::ForegroundTabLabel(Cursor.ToSharedRef(), 0);
			if (!Label.IsEmpty())
			{
				Labels.Insert(Label, 0);
			}
		}
		Cursor = Cursor->GetParentWidget();
	}
	return FString::Join(Labels, TEXT(">"));
}

bool GameTestEditorSlateFind::GetCenter(const SWidget& InWidget, FVector2D& OutCenter)
{
	const FGeometry& Geometry = InWidget.GetCachedGeometry();
	const FVector2D Position = FVector2D(Geometry.GetAbsolutePosition());
	const FVector2D Size = FVector2D(Geometry.GetAbsoluteSize());
	if (Size.X < 1.0 || Size.Y < 1.0)
	{
		return false;
	}
	OutCenter = Position + Size * 0.5;
	return true;
}

void GameTestEditorSlateFind::Find(const FGameTestEditorWidgetSpec& InSpec, TArray<TSharedRef<SWidget>>& OutWidgets)
{
	OutWidgets.Reset();
	if (!FSlateApplication::IsInitialized() || InSpec.Widget.IsEmpty())
	{
		return;
	}
	TArray<TSharedRef<SWindow>> Windows;
	for (const TSharedRef<SWindow>& Window : FSlateApplication::Get().GetInteractiveTopLevelWindows())
	{
		GameTestEditorSlateFindDetail::CollectWindows(Window, Windows);
	}

	GameTestEditorSlateFindDetail::FFindState State;
	State.Spec = &InSpec;
	State.Type = FName(*InSpec.Widget);
	State.Out = &OutWidgets;
	for (const TSharedRef<SWindow>& Window : Windows)
	{
		if (InSpec.Tab.IsEmpty() && !InSpec.Window.IsEmpty() && !Window->GetTitle().ToString().Contains(InSpec.Window))
		{
			continue;
		}
		GameTestEditorSlateFindDetail::Visit(Window, State);
	}
}

bool GameTestEditorSlateFind::Describe(const TSharedRef<SWidget>& InWidget, const TSharedPtr<SWindow>& InWindow, FGameTestEditorWidgetSpec& OutSpec)
{
	OutSpec = FGameTestEditorWidgetSpec();
	OutSpec.Widget = InWidget->GetTypeAsString();
	OutSpec.Text = GameTestEditorSlateFindDetail::TextOf(InWidget);
	OutSpec.Tab = TabChain(InWidget);
	if (OutSpec.Tab.IsEmpty() && InWindow.IsValid())
	{
		OutSpec.Window = InWindow->GetTitle().ToString();
	}

	// 第几个按回放时的查找顺序算：同一页里有几个同字的按钮时，回放点到的就是录的这一个
	TArray<TSharedRef<SWidget>> Matches;
	Find(OutSpec, Matches);
	const int32 Index = Matches.IndexOfByPredicate([&InWidget](const TSharedRef<SWidget>& InCandidate)
	{
		return &InCandidate.Get() == &InWidget.Get();
	});
	OutSpec.Nth = FMath::Max(Index, 0);
	return Index != INDEX_NONE;
}
