// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestKeyPreview.h"

#include "Components/Widget.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Rendering/DrawElements.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"

namespace GameTestKeyPreviewDetail
{
	const FLinearColor UniqueColor(0.30f, 0.85f, 0.45f, 1.0f);
	const FLinearColor ManyColor(1.0f, 0.62f, 0.20f, 1.0f);
	constexpr int32 PreviewZOrder = 1001;

	/** 画的数据：层和持有者共享，层多活几帧也读不到释放的内存。 */
	struct FPaint
	{
		TArray<TWeakObjectPtr<UWidget>> Widgets;
	};

	class SKeyPreviewOverlay : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SKeyPreviewOverlay) {}
			SLATE_ARGUMENT(TSharedPtr<FPaint>, Paint)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			Paint = InArgs._Paint;
		}

		virtual FVector2D ComputeDesiredSize(float) const override
		{
			return FVector2D::ZeroVector;
		}

		virtual int32 OnPaint(const FPaintArgs& InArgs, const FGeometry& InAllottedGeometry, const FSlateRect& InMyCullingRect,
			FSlateWindowElementList& OutDrawElements, const int32 InLayerId, const FWidgetStyle& InWidgetStyle, const bool bInParentEnabled) const override
		{
			const FPaint* PaintData = Paint.Get();
			if (PaintData == nullptr || PaintData->Widgets.Num() == 0)
			{
				return InLayerId;
			}
			const FLinearColor Color = PaintData->Widgets.Num() == 1 ? UniqueColor : ManyColor;
			const FPaintGeometry PaintGeometry = InAllottedGeometry.ToPaintGeometry();
			for (const TWeakObjectPtr<UWidget>& Weak : PaintData->Widgets)
			{
				const UWidget* Widget = Weak.Get();
				const TSharedPtr<SWidget> Slate = Widget != nullptr ? Widget->GetCachedWidget() : nullptr;
				if (!Slate.IsValid())
				{
					continue;
				}
				// 控件的缓存几何和这一层都在桌面坐标里：AbsoluteToLocal 一次就消掉窗口位置和 DPI 缩放
				const FGeometry& Geometry = Slate->GetCachedGeometry();
				const FVector2D AbsMin = FVector2D(Geometry.GetAbsolutePosition());
				const FVector2D AbsMax = AbsMin + FVector2D(Geometry.GetAbsoluteSize());
				const FVector2D Min = InAllottedGeometry.AbsoluteToLocal(AbsMin);
				const FVector2D Max = InAllottedGeometry.AbsoluteToLocal(AbsMax);
				const TArray<FVector2D> Outline = {
					FVector2D(Min.X, Min.Y), FVector2D(Max.X, Min.Y), FVector2D(Max.X, Max.Y), FVector2D(Min.X, Max.Y), FVector2D(Min.X, Min.Y),
				};
				FSlateDrawElement::MakeLines(OutDrawElements, InLayerId, PaintGeometry, Outline, ESlateDrawEffect::None, Color, /*bAntialias=*/true, /*Thickness=*/3.0f);
			}
			return InLayerId + 1;
		}

	private:
		TSharedPtr<FPaint> Paint;
	};

	struct FHolder
	{
		TSharedPtr<SWidget> Overlay;
		TSharedPtr<FPaint> Paint;
		TWeakObjectPtr<UGameViewportClient> Viewport;
		FTSTicker::FDelegateHandle Ticker;
		double ExpiresAt = 0.0;
	};

	TUniquePtr<FHolder> Holder;
}

void GameTestKeyPreview::Clear()
{
	using namespace GameTestKeyPreviewDetail;
	if (!Holder.IsValid())
	{
		return;
	}
	if (Holder->Ticker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(Holder->Ticker);
	}
	if (UGameViewportClient* Viewport = Holder->Viewport.Get())
	{
		if (Holder->Overlay.IsValid())
		{
			Viewport->RemoveViewportWidgetContent(Holder->Overlay.ToSharedRef());
		}
	}
	if (Holder->Paint.IsValid())
	{
		Holder->Paint->Widgets.Reset();
	}
	Holder.Reset();
}

void GameTestKeyPreview::Show(const TArray<UWidget*>& InWidgets, double InSeconds)
{
	using namespace GameTestKeyPreviewDetail;
	Clear();
	UGameViewportClient* Viewport = GEngine != nullptr ? GEngine->GameViewport : nullptr;
	if (Viewport == nullptr || InWidgets.Num() == 0)
	{
		// 没有游戏视口（没起 PIE、无头）：没什么可框的，命中数照样由调用方返回
		return;
	}

	Holder = MakeUnique<FHolder>();
	Holder->Paint = MakeShared<FPaint>();
	for (UWidget* Widget : InWidgets)
	{
		if (Widget != nullptr)
		{
			Holder->Paint->Widgets.Add(Widget);
		}
	}
	Holder->Overlay = SNew(SOverlay)
		.Visibility(EVisibility::HitTestInvisible)
		+ SOverlay::Slot()
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Fill)
		[
			SNew(SKeyPreviewOverlay).Paint(Holder->Paint)
		];
	Viewport->AddViewportWidgetContent(Holder->Overlay.ToSharedRef(), PreviewZOrder);
	Holder->Viewport = Viewport;
	Holder->ExpiresAt = FPlatformTime::Seconds() + FMath::Clamp(InSeconds, 0.5, 30.0);
	// 游戏暂停时世界不 tick，用核心 ticker 计时和重画
	Holder->Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
	{
		if (!Holder.IsValid())
		{
			return false;
		}
		if (FPlatformTime::Seconds() > Holder->ExpiresAt || !Holder->Viewport.IsValid())
		{
			// Clear 里会摘掉这个 ticker；返回 false 也让 ticker 自己不再调
			Holder->Ticker.Reset();
			Clear();
			return false;
		}
		if (Holder->Overlay.IsValid())
		{
			Holder->Overlay->Invalidate(EInvalidateWidgetReason::Paint);
		}
		return true;
	}));
}
