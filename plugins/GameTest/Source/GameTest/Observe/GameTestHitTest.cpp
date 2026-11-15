// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestHitTest.h"

#include "Observe/GameTestSlate.h"

#include "Application/SlateApplicationBase.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Input/HittestGrid.h"
#include "Layout/ArrangedWidget.h"
#include "Layout/WidgetPath.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

FString GameTestHitTest::DescribeOwningWidget(const SWidget& InSlateWidget)
{
	// 先查 Slate 控件上的反射元数据（UMG 建控件时挂的），查不到才在 UMG 对象里按指针比对（GameTestSlate 里有退路）。
	// 回放的自动等待每帧都会探一次，逐个线性扫描会很慢。
	const UWidget* Widget = GameTestSlate::FindSourceWidget(InSlateWidget);
	if (Widget == nullptr)
	{
		return FString();
	}

	if (const UUserWidget* Owner = Widget->GetTypedOuter<UUserWidget>())
	{
		return FString::Printf(TEXT("%s(%s)@%s"),
			*Widget->GetClass()->GetName(), *Widget->GetFName().ToString(), *Owner->GetClass()->GetName());
	}

	return FString::Printf(TEXT("%s(%s)"), *Widget->GetClass()->GetName(), *Widget->GetFName().ToString());
}

bool GameTestHitTest::Probe(UWidget& InWidget, const FVector2D& InPoint, FResult& OutResult, FString& OutError)
{
	OutResult = FResult();

	const TSharedPtr<SWidget> SlateWidget = InWidget.GetCachedWidget();
	if (!SlateWidget.IsValid())
	{
		OutError = FString::Printf(TEXT("%s 还没有 Slate 控件（界面没被构建，或从未挂上视口）"),
			*InWidget.GetFName().ToString());
		return false;
	}

	return ProbeSlate(SlateWidget.ToSharedRef(), InPoint, OutResult, OutError);
}

bool GameTestHitTest::ProbeSlate(const TSharedRef<SWidget>& InSlateWidget, const FVector2D& InPoint, FResult& OutResult, FString& OutError)
{
	OutResult = FResult();
	const TSharedPtr<SWidget> SlateWidget = InSlateWidget;

	// 与快照取几何同一条链（Observe/GameTestSnapshot.cpp 的 bHasGeometry 判据一致：UWidget::GetCachedGeometry 就是它的 Slate 控件的）。
	const FGeometry& Geometry = SlateWidget->GetCachedGeometry();
	const UE::Slate::FDeprecateVector2DResult LocalSize = Geometry.GetLocalSize();
	if (LocalSize.X < 1.0 || LocalSize.Y < 1.0)
	{
		OutError = FString::Printf(TEXT("%s 的几何无效（Collapsed、从未布局，或还没刷过一帧）"),
			*SlateWidget->GetTypeAsString());
		return false;
	}

	// 越界的点等于探到控件外面，容易得到"看起来像 bug"的结论——收进 0–1 并如实回报。
	OutResult.Point = FVector2D(FMath::Clamp(InPoint.X, 0.0, 1.0), FMath::Clamp(InPoint.Y, 0.0, 1.0));

	// 控件内归一化点 → 窗口空间（Absolute）→ 虚拟桌面空间（hittest grid 的坐标系）。
	const FVector2D LocalPoint(LocalSize.X * OutResult.Point.X, LocalSize.Y * OutResult.Point.Y);
	const UE::Slate::FDeprecateVector2DResult WindowPoint = Geometry.LocalToAbsolute(LocalPoint);

	const TSharedPtr<SWindow> Window = FSlateApplicationBase::Get().FindWidgetWindow(SlateWidget.ToSharedRef());
	if (!Window.IsValid())
	{
		OutError = FString::Printf(TEXT("%s 不在任何窗口里（界面挂上视口了吗）"), *SlateWidget->GetTypeAsString());
		return false;
	}

	const UE::Slate::FDeprecateVector2DResult WindowPosition = Window->GetPositionInScreen();
	const UE::Slate::FDeprecateVector2DResult WindowSize = Window->GetSizeInScreen();

	// 命中测试要的坐标是**窗口内的绝对坐标**（Slate 单位）。
	// 引擎 `GetBubblePath` 的注释写的是"虚拟桌面空间"，但实测（2560x1440 @ 150% DPI）：
	// 「 Absolute + 窗口位置 」在 grid 里查不到，纯窗口内坐标才对——grid 按窗口记录，不再叠窗口原点。
	FHittestGrid& Grid = Window->GetHittestGrid();
	TArray<FWidgetAndPointer> WidgetsAndCursors = Grid.GetBubblePath(
		WindowPoint, /*InCursorRadius=*/0.f, /*bIgnoreEnabledStatus=*/false, INDEX_NONE);
	const FWidgetPath Path(MoveTemp(WidgetsAndCursors));
	if (!Path.IsValid())
	{
		// 坐标链一次给全：坐标系是实测调出来的，出偏差时没有这串数字根本猜不动。
		const UE::Slate::FDeprecateVector2DResult CursorPosition = FSlateApplicationBase::Get().GetCursorPos();
		OutError = FString::Printf(
			TEXT("命中测试没有返回路径：窗口内点 (%.0f, %.0f) 处没有可命中的控件（控件内 %.0f, %.0f；")
			TEXT("窗口位置 %.0f, %.0f、尺寸 %.0f, %.0f、DPI %.2f；光标 %.0f, %.0f；可见=%s、接受输入=%s）"),
			WindowPoint.X, WindowPoint.Y,
			LocalPoint.X, LocalPoint.Y,
			WindowPosition.X, WindowPosition.Y,
			WindowSize.X, WindowSize.Y,
			Window->GetDPIScaleFactor(),
			CursorPosition.X, CursorPosition.Y,
			Window->IsVisible() ? TEXT("是") : TEXT("否"),
			Window->AcceptsInput() ? TEXT("是") : TEXT("否"));
		return false;
	}

	for (int32 Index = 0; Index < Path.Widgets.Num(); ++Index)
	{
		const TSharedPtr<SWidget> Item = Path.Widgets[Index].Widget;
		if (!Item.IsValid())
		{
			continue;
		}

		const FString Owner = DescribeOwningWidget(*Item);
		OutResult.HitPath.Add(Owner.IsEmpty()
			? Item->GetTypeAsString()
			: FString::Printf(TEXT("%s → %s"), *Item->GetTypeAsString(), *Owner));
	}

	OutResult.TopSlateWidget = Path.GetLastWidget();
	OutResult.TopDescription = OutResult.HitPath.Num() > 0
		? OutResult.HitPath.Last()
		: OutResult.TopSlateWidget->GetTypeAsString();
	OutResult.bTargetHit = Path.ContainsWidget(SlateWidget.Get());
	return true;
}
