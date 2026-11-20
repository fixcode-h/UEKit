// Copyright Epic Games, Inc. All Rights Reserved.

// gt.highlight（docs/game-test-ref.md 7.2）：在游戏视口上画一层不接输入的覆盖层，给面板树里悬停 / 选中的控件描边。
// 不改游戏的控件；每帧按控件此刻的几何重画（控件在动画、列表在滚也跟得住）。租约同 gt.pick：60 秒没再调就自动清。

#include "Integration/Ref/GameTestRpcRef.h"

#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Ref/GameTestPropPath.h"
#include "Observe/Ref/GameTestRefResolve.h"

#include "Components/Widget.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Fonts/SlateFontInfo.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateLayoutTransform.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"

namespace GameTestRpcHighlightImpl
{
	constexpr double LeaseSeconds = 60.0;
	/** 比执行器的高亮层（1000）再高一层：两者同时在时描边在上面。 */
	constexpr int32 OverlayZOrder = 1001;

	struct FBox
	{
		TWeakObjectPtr<UWidget> Widget;
		FString Label;
	};

	struct FPaint
	{
		TArray<FBox> Boxes;
		FLinearColor Color = FLinearColor(FColor(0xd9, 0x77, 0x57));
	};

	class SGameTestRefHighlight : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SGameTestRefHighlight) {}
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

		virtual int32 OnPaint(const FPaintArgs& InArgs, const FGeometry& InAllottedGeometry, const FSlateRect& InMyCullingRect, FSlateWindowElementList& OutDrawElements, const int32 InLayerId, const FWidgetStyle& InWidgetStyle, const bool bInParentEnabled) const override
		{
			if (!Paint.IsValid())
			{
				return InLayerId;
			}
			const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9);
			for (const FBox& Box : Paint->Boxes)
			{
				const UWidget* Widget = Box.Widget.Get();
				if (Widget == nullptr || !Widget->GetCachedWidget().IsValid())
				{
					continue;
				}
				// 控件的几何是绝对坐标：两个角都过 AbsoluteToLocal（同时消掉窗口平移和 DPI 缩放，见执行器高亮层的说明）
				const FGeometry& Geometry = Widget->GetCachedGeometry();
				const FVector2D AbsMin = FVector2D(Geometry.GetAbsolutePosition());
				const FVector2D AbsMax = AbsMin + FVector2D(Geometry.GetAbsoluteSize());
				const FVector2D Min = FVector2D(InAllottedGeometry.AbsoluteToLocal(AbsMin));
				const FVector2D Max = FVector2D(InAllottedGeometry.AbsoluteToLocal(AbsMax));
				const TArray<FVector2D> Outline = { FVector2D(Min.X, Min.Y), FVector2D(Max.X, Min.Y), FVector2D(Max.X, Max.Y), FVector2D(Min.X, Max.Y), FVector2D(Min.X, Min.Y) };
				FSlateDrawElement::MakeLines(OutDrawElements, InLayerId, InAllottedGeometry.ToPaintGeometry(), Outline, ESlateDrawEffect::None, Paint->Color, true, 2.0f);
				if (!Box.Label.IsEmpty())
				{
					const FVector2D LabelAt(Min.X + 2.0, FMath::Max(0.0, Min.Y - 14.0));
					FSlateDrawElement::MakeText(OutDrawElements, InLayerId + 1, InAllottedGeometry.ToPaintGeometry(FVector2D(400.0, 14.0), FSlateLayoutTransform(LabelAt)), Box.Label, Font, ESlateDrawEffect::None, Paint->Color);
				}
			}
			return InLayerId + 2;
		}

	private:
		TSharedPtr<FPaint> Paint;
	};

	struct FState
	{
		TSharedPtr<FPaint> Paint;
		TSharedPtr<SWidget> Overlay;
		TWeakObjectPtr<UGameViewportClient> Viewport;
		/** 到这个时间清（ms 给了时）；0 = 一直描到下次。 */
		double ExpiresAt = 0.0;
		double LeaseUntil = 0.0;
		FTSTicker::FDelegateHandle Ticker;
	};

	FState& State()
	{
		static FState Instance;
		return Instance;
	}

	void Clear()
	{
		FState& S = State();
		if (S.Overlay.IsValid())
		{
			if (UGameViewportClient* Viewport = S.Viewport.Get())
			{
				Viewport->RemoveViewportWidgetContent(S.Overlay.ToSharedRef());
			}
		}
		if (S.Paint.IsValid())
		{
			S.Paint->Boxes.Reset();
		}
		S.Overlay.Reset();
		S.Viewport.Reset();
		if (S.Ticker.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(S.Ticker);
			S.Ticker.Reset();
		}
	}

	bool Tick(float)
	{
		FState& S = State();
		const double Now = FPlatformTime::Seconds();
		const bool bViewportGone = !S.Viewport.IsValid() || (GEngine != nullptr && GEngine->GameViewport != S.Viewport.Get());
		if (bViewportGone || Now > S.LeaseUntil || (S.ExpiresAt > 0.0 && Now > S.ExpiresAt))
		{
			// 返回 false 让 ticker 自己摘掉这一项：先把句柄清了，Clear 里就不再 RemoveTicker
			S.Ticker.Reset();
			Clear();
			return false;
		}
		if (S.Overlay.IsValid())
		{
			S.Overlay->Invalidate(EInvalidateWidgetReason::Paint);
		}
		return true;
	}

	bool ParseColor(const FString& InText, FLinearColor& OutColor)
	{
		if (InText.IsEmpty())
		{
			return false;
		}
		OutColor = FLinearColor(FColor::FromHex(InText));
		return true;
	}
}

void GameTestRpcRef::Shutdown()
{
	GameTestRpcHighlightImpl::Clear();
}

bool GameTestRpcRef::Highlight(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
{
	using namespace GameTestRpcHighlightImpl;
	OutResult = MakeShared<FJsonObject>();
	bool bClear = false;
	if (!InArgs.IsValid() || (InArgs->TryGetBoolField(TEXT("clear"), bClear) && bClear))
	{
		Clear();
		OutResult->SetBoolField(TEXT("cleared"), true);
		return true;
	}
	TArray<FBox> Boxes;
	FString NodeId;
	if (InArgs->TryGetStringField(TEXT("node"), NodeId) && !NodeId.IsEmpty())
	{
		UWidget* Widget = FindNode(NodeId);
		if (Widget == nullptr)
		{
			OutError = FString::Printf(TEXT("没有节点 %s（gt.tree 每次调用重新编号，控件没了 id 也失效）"), *NodeId);
			return false;
		}
		Boxes.Add({ Widget, Widget->GetClass()->GetName() });
	}
	else if (InArgs->HasField(TEXT("target")))
	{
		// 部件写法解析到的全部描出来（歧义时每个候选都描，多个时编号）
		FGameTestFlowContext Flow;
		MakeContext(Flow);
		const FGameTestTargetResult Result = GameTestRefResolve::ResolveTarget(InArgs->TryGetField(TEXT("target")), Flow);
		TArray<FGameTestRefCandidate> Candidates = Result.Ref.Matches;
		if (Candidates.Num() == 0 && Result.Ref.IsOk())
		{
			Candidates.Add(Result.Ref.Picked);
		}
		for (int32 Index = 0; Index < Candidates.Num(); ++Index)
		{
			FString PartError;
			UWidget* Widget = Cast<UWidget>(GameTestPropRead::FindPart(Candidates[Index].Object.Get(), Result.Target.Part, PartError));
			if (Widget != nullptr)
			{
				const FString Name = Widget->GetClass()->GetName();
				Boxes.Add({ Widget, Candidates.Num() > 1 ? FString::Printf(TEXT("%d %s"), Index + 1, *Name) : Name });
			}
		}
		if (Boxes.Num() == 0)
		{
			OutError = Result.Error.IsEmpty() ? FString(TEXT("target 没解析到界面控件")) : Result.Error;
			return false;
		}
	}
	else
	{
		OutError = TEXT("gt.highlight 要 node（gt.tree 的 id）、target（部件写法）或 clear: true");
		return false;
	}

	UGameViewportClient* Viewport = GEngine != nullptr ? GEngine->GameViewport : nullptr;
	if (Viewport == nullptr)
	{
		OutError = TEXT("没有游戏视口（先开游戏）");
		return false;
	}
	FState& S = State();
	if (S.Viewport.Get() != Viewport)
	{
		Clear();
	}
	if (!S.Paint.IsValid())
	{
		S.Paint = MakeShared<FPaint>();
	}
	S.Paint->Boxes = Boxes;
	FString ColorText;
	FLinearColor Color(FColor(0xd9, 0x77, 0x57));
	if (InArgs->TryGetStringField(TEXT("color"), ColorText))
	{
		ParseColor(ColorText, Color);
	}
	S.Paint->Color = Color;
	if (!S.Overlay.IsValid())
	{
		S.Overlay = SNew(SOverlay).Visibility(EVisibility::HitTestInvisible)
			+ SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill)[SNew(SGameTestRefHighlight).Paint(S.Paint)];
		Viewport->AddViewportWidgetContent(S.Overlay.ToSharedRef(), OverlayZOrder);
		S.Viewport = Viewport;
	}
	const double Now = FPlatformTime::Seconds();
	double Ms = 0.0;
	InArgs->TryGetNumberField(TEXT("ms"), Ms);
	S.ExpiresAt = Ms > 0.0 ? Now + Ms / 1000.0 : 0.0;
	S.LeaseUntil = Now + LeaseSeconds;
	if (!S.Ticker.IsValid())
	{
		S.Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick), 0.0f);
	}
	OutResult->SetNumberField(TEXT("count"), Boxes.Num());
	return true;
}
