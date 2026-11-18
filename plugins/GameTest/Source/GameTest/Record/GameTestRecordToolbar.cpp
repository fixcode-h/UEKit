// Copyright Epic Games, Inc. All Rights Reserved.

#include "Record/GameTestRecordToolbar.h"

#include "Core/GameTestWorld.h"
#include "Record/GameTestRecorder.h"
#include "GameTest.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Dom/JsonObject.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericApplication.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace GameTestRecordToolbarDetail
{
	const FLinearColor TextColor(0.92f, 0.92f, 0.94f, 1.0f);
	const FLinearColor DimColor(0.62f, 0.64f, 0.68f, 1.0f);
	const FLinearColor RecordColor(0.94f, 0.33f, 0.31f, 1.0f);
	const FLinearColor MoveColor(0.35f, 0.68f, 0.98f, 1.0f);
	const FLinearColor NoticeColor(0.98f, 0.76f, 0.35f, 1.0f);

	/** 默认位置离视口底边的距离（Slate 单位，按视口的 DPI 缩放）：留出游戏底部常见的技能栏、聊天框边缘，又不至于飘到画面中间。 */
	constexpr double BottomGap = 56.0;

	/** 把手：两道竖线，告诉用户这里可以按住拖。不接点击，按下落到外层的工具栏底上处理。 */
	TSharedRef<SWidget> MakeGrip()
	{
		static const FSlateRoundedBoxBrush GripBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.28f), 1.0f);
		auto Line = []()
		{
			return SNew(SBox).WidthOverride(2.0f).HeightOverride(14.0f)
				[
					SNew(SBorder).BorderImage(&GripBrush).Padding(0.0f)
				];
		};
		return SNew(SHorizontalBox)
			.Visibility(EVisibility::HitTestInvisible)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center) [ Line() ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f, 0.0f, 0.0f) [ Line() ];
	}

	/** 深色底，和面板深色主题的浮层接近；不依赖编辑器样式，独立游戏进程里也有。窗口本身不透明，所以底色也不透明。 */
	const FSlateBrush* BarBrush()
	{
		static const FSlateRoundedBoxBrush Brush(FLinearColor(0.07f, 0.07f, 0.09f, 1.0f), 6.0f, FLinearColor(1.0f, 1.0f, 1.0f, 0.12f), 1.0f);
		return &Brush;
	}

	const FButtonStyle& ButtonStyle()
	{
		static const FButtonStyle Style = []
		{
			FButtonStyle Out;
			Out.SetNormal(FSlateRoundedBoxBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.06f), 6.0f));
			Out.SetHovered(FSlateRoundedBoxBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.14f), 6.0f));
			Out.SetPressed(FSlateRoundedBoxBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.22f), 6.0f));
			Out.SetNormalPadding(FMargin(8.0f, 4.0f));
			Out.SetPressedPadding(FMargin(8.0f, 5.0f, 8.0f, 3.0f));
			return Out;
		}();
		return Style;
	}

	FSlateFontInfo Font(int32 InSize, bool bInBold = false)
	{
		return FCoreStyle::GetDefaultFontStyle(bInBold ? "Bold" : "Regular", InSize);
	}

	/** 按钮：点了走录制器的同一个入口（和快捷键、面板按钮一样），出错显示在工具栏的提示行。 */
	TSharedRef<SWidget> MakeButton(const FString& InLabel, const FString& InKeys, TSharedRef<FGameTestRecordToolbar::FState> InState, TFunction<void()> InOnClick)
	{
		return SNew(SButton)
			.ButtonStyle(&ButtonStyle())
			// 按钮不拿键盘焦点：点完焦点还在游戏上，录按键、Alt+数字不受影响。
			.IsFocusable(false)
			.OnClicked_Lambda([InOnClick]()
			{
				InOnClick();
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(FText::FromString(InLabel)).Font(Font(9)).ColorAndOpacity(TextColor)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(5.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(InKeys))
					.Font(Font(8))
					.ColorAndOpacity(DimColor)
					.Visibility_Lambda([InState]() { return InState->bHotkeys ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
				]
			];
	}

	void RunQuick(const TCHAR* InPhase)
	{
		FString Error;
		FGameTestRecorder::Get().Quick(InPhase, 0.0, Error);
	}
}

void FGameTestRecordToolbar::Sync(const FState& InState)
{
	*State = InState;

	UGameViewportClient* Current = GameTestWorld::FindGameViewport();
	if (Current != Viewport.Get() || !Window.IsValid())
	{
		Hide();
		if (Current == nullptr || !Open(Current))
		{
			return;
		}
	}
	FollowViewport();
}

bool FGameTestRecordToolbar::Open(UGameViewportClient* InViewport)
{
	if (InViewport == nullptr || !FSlateApplication::IsInitialized())
	{
		return false;
	}
	// 父窗口取游戏视口所在的窗口：PIE 在关卡视口里时是编辑器主窗口，单独窗口 PIE / 独立游戏时是游戏窗口。
	// 做成它的子窗口：始终在它上面、跟着它最小化，父窗口关了（停止 PIE）子窗口一起关。
	TSharedPtr<SWindow> Parent;
	const TSharedPtr<SViewport> ViewportWidget = InViewport->GetGameViewportWidget();
	if (ViewportWidget.IsValid())
	{
		Parent = FSlateApplication::Get().FindWidgetWindow(ViewportWidget.ToSharedRef());
	}
	if (!Parent.IsValid())
	{
		Parent = InViewport->GetWindow();
	}
	if (!Parent.IsValid())
	{
		return false;
	}

	bPlaced = false;
	bDragging = false;
	const TSharedRef<SWindow> NewWindow = SNew(SWindow)
		.Title(FText::FromString(TEXT("GameTest 录制")))
		.Type(EWindowType::Normal)
		.SizingRule(ESizingRule::Autosized)
		.CreateTitleBar(false)
		.UseOSWindowBorder(false)
		.HasCloseButton(false)
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		.LayoutBorder(FMargin(0.0f))
		.UserResizeBorder(FMargin(0.0f))
		.SaneWindowPlacement(false)
		// 不抢焦点：点工具栏时游戏窗口保持激活，键盘焦点留在游戏上。
		.ActivationPolicy(EWindowActivationPolicy::Never)
		.FocusWhenFirstShown(false)
		[
			Build()
		];
	Window = NewWindow;
	Viewport = InViewport;

	// 父窗口关掉时子窗口跟着关：清掉引用，下一帧 Sync 按需重开。
	const TWeakPtr<FGameTestRecordToolbar> WeakSelf = AsShared();
	NewWindow->SetOnWindowClosed(FOnWindowClosed::CreateLambda([WeakSelf](const TSharedRef<SWindow>& InClosed)
	{
		const TSharedPtr<FGameTestRecordToolbar> This = WeakSelf.Pin();
		if (This.IsValid() && This->Window.Get() == &InClosed.Get())
		{
			This->Window.Reset();
			This->Bar.Reset();
			This->bDragging = false;
		}
	}));
	FSlateApplication::Get().AddWindowAsNativeChild(NewWindow, Parent.ToSharedRef(), /*bShowImmediately=*/true);
	FollowViewport();
	return true;
}

void FGameTestRecordToolbar::FollowViewport()
{
	using namespace GameTestRecordToolbarDetail;
	if (bPlaced || bDragging || !Window.IsValid())
	{
		return;
	}
	UGameViewportClient* Current = Viewport.Get();
	const TSharedPtr<SViewport> ViewportWidget = Current != nullptr ? Current->GetGameViewportWidget() : nullptr;
	if (!ViewportWidget.IsValid())
	{
		return;
	}
	// 视口的屏幕位置和大小（屏幕像素）；第一次排版前是 0，等下一帧。
	const FGeometry& Geometry = ViewportWidget->GetCachedGeometry();
	const FVector2D ViewportPos = FVector2D(Geometry.GetAbsolutePosition());
	const FVector2D ViewportSize = FVector2D(Geometry.GetAbsoluteSize());
	if (ViewportSize.X <= 0.0 || ViewportSize.Y <= 0.0)
	{
		return;
	}
	const FVector2D WindowSize = FVector2D(Window->GetSizeInScreen());
	const FVector2D Target(
		FMath::RoundToDouble(ViewportPos.X + (ViewportSize.X - WindowSize.X) * 0.5),
		FMath::RoundToDouble(ViewportPos.Y + ViewportSize.Y - WindowSize.Y - BottomGap * double(Geometry.Scale)));
	// 位置没变就不动：MoveWindowTo 每帧调会让系统窗口一直重绘。
	if (FVector2D::Distance(Target, FVector2D(Window->GetPositionInScreen())) > 0.5)
	{
		Window->MoveWindowTo(Target);
	}
}

void FGameTestRecordToolbar::Hide()
{
	const TSharedPtr<SWindow> Old = Window;
	Window.Reset();
	Bar.Reset();
	Viewport.Reset();
	bDragging = false;
	if (Old.IsValid() && FSlateApplication::IsInitialized())
	{
		Old->RequestDestroyWindow();
	}
}

bool FGameTestRecordToolbar::Contains(const FVector2D& InScreenPosition) const
{
	if (!Window.IsValid() || !Window->IsVisible())
	{
		return false;
	}
	return Window->GetRectInScreen().ContainsPoint(InScreenPosition);
}

TSharedRef<SWidget> FGameTestRecordToolbar::Build()
{
	using namespace GameTestRecordToolbarDetail;
	const TSharedRef<FState> StateRef = State;
	const TWeakPtr<FGameTestRecordToolbar> WeakSelf = AsShared();

	auto IdleVisible = [StateRef]() { return StateRef->bMoving ? EVisibility::Collapsed : EVisibility::SelfHitTestInvisible; };
	auto MovingVisible = [StateRef]() { return StateRef->bMoving ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed; };

	const TSharedRef<SWidget> Idle = SNew(SHorizontalBox)
		.Visibility_Lambda(IdleVisible)
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f)
		[ MakeButton(TEXT("录移动"), TEXT("Alt+1"), StateRef, [] { RunQuick(TEXT("start")); }) ]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f)
		[ MakeButton(TEXT("记下位置"), TEXT("Alt+2"), StateRef, [] { RunQuick(TEXT("here")); }) ]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f)
		[ MakeButton(TEXT("在这附近"), TEXT("Alt+3"), StateRef, [] { RunQuick(TEXT("near")); }) ]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f)
		[ MakeButton(TEXT("断言"), TEXT("Alt+4"), StateRef, [] { RunQuick(TEXT("pick")); }) ]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f)
		[ MakeButton(TEXT("检查点"), TEXT("Alt+5"), StateRef, [] { FString Error; FGameTestRecorder::Get().Mark(FString(), false, Error); }) ]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f)
		[ MakeButton(TEXT("停止"), FString(), StateRef, []
			{
				TSharedPtr<FJsonObject> Result;
				FString Error;
				FGameTestRecorder::Get().Stop(/*bInSave=*/true, Result, Error);
			}) ];

	const TSharedRef<SWidget> Moving = SNew(SHorizontalBox)
		.Visibility_Lambda(MovingVisible)
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f)
		[ MakeButton(TEXT("结束移动"), TEXT("Alt+1"), StateRef, [] { RunQuick(TEXT("end")); }) ]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f)
		[ MakeButton(TEXT("放弃"), TEXT("Alt+0"), StateRef, [] { RunQuick(TEXT("cancel")); }) ];

	const TSharedRef<SBorder> BarWidget = SNew(SBorder)
		.BorderImage(BarBrush())
		.Padding(FMargin(8.0f, 6.0f, 10.0f, 6.0f))
		.Cursor(EMouseCursor::GrabHand)
		.OnMouseButtonDown_Lambda([WeakSelf](const FGeometry&, const FPointerEvent& InEvent)
		{
			// 按在把手、状态文字或工具栏空白处开始拖整个窗口：按钮自己接住点击，到不了这里。
			// 录制器那边看到按下点在工具栏上就不录，配对的松开也跳过，所以拖动不会变成步骤。
			const TSharedPtr<FGameTestRecordToolbar> This = WeakSelf.Pin();
			if (!This.IsValid() || !This->Bar.IsValid() || !This->Window.IsValid() || InEvent.GetEffectingButton() != EKeys::LeftMouseButton)
			{
				return FReply::Unhandled();
			}
			FDisplayMetrics Metrics;
			FSlateApplication::Get().GetCachedDisplayMetrics(Metrics);
			const FPlatformRect& Desktop = Metrics.VirtualDisplayRect;
			This->DesktopMin = FVector2D(double(Desktop.Left), double(Desktop.Top));
			This->DesktopMax = FVector2D(double(Desktop.Right), double(Desktop.Bottom));
			This->GrabOffset = FVector2D(InEvent.GetScreenSpacePosition()) - FVector2D(This->Window->GetPositionInScreen());
			This->bPlaced = true;
			This->bDragging = true;
			return FReply::Handled().CaptureMouse(This->Bar.ToSharedRef());
		})
		.OnMouseMove_Lambda([WeakSelf](const FGeometry&, const FPointerEvent& InEvent)
		{
			const TSharedPtr<FGameTestRecordToolbar> This = WeakSelf.Pin();
			if (!This.IsValid() || !This->bDragging || !This->Window.IsValid())
			{
				return FReply::Unhandled();
			}
			FVector2D Target = FVector2D(InEvent.GetScreenSpacePosition()) - This->GrabOffset;
			// 只夹在整个桌面里（可以拖出游戏视口、拖到别的显示器），不让窗口整个出屏幕找不回来。
			const FVector2D Size = FVector2D(This->Window->GetSizeInScreen());
			if (This->DesktopMax.X > This->DesktopMin.X && This->DesktopMax.Y > This->DesktopMin.Y)
			{
				Target.X = FMath::Clamp(Target.X, This->DesktopMin.X, FMath::Max(This->DesktopMin.X, This->DesktopMax.X - Size.X));
				Target.Y = FMath::Clamp(Target.Y, This->DesktopMin.Y, FMath::Max(This->DesktopMin.Y, This->DesktopMax.Y - Size.Y));
			}
			This->Window->MoveWindowTo(Target);
			return FReply::Handled();
		})
		.OnMouseButtonUp_Lambda([WeakSelf](const FGeometry&, const FPointerEvent&)
		{
			const TSharedPtr<FGameTestRecordToolbar> This = WeakSelf.Pin();
			if (!This.IsValid() || !This->bDragging)
			{
				return FReply::Unhandled();
			}
			This->bDragging = false;
			return FReply::Handled().ReleaseMouseCapture();
		})
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					MakeGrip()
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("●")))
					.Font(Font(10))
					.ColorAndOpacity_Lambda([StateRef]() { return FSlateColor(StateRef->bMoving ? MoveColor : RecordColor); })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([StateRef]() { return FText::FromString(StateRef->Status); })
					.Font(Font(9, /*bInBold=*/true))
					.ColorAndOpacity(TextColor)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center) [ Idle ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center) [ Moving ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([StateRef]()
				{
					return FText::FromString(StateRef->bPickArmed ? FString(TEXT("下一次点击录成断言")) : StateRef->Notice);
				})
				.Font(Font(8))
				.ColorAndOpacity(NoticeColor)
				.Visibility_Lambda([StateRef]()
				{
					return (StateRef->bPickArmed || !StateRef->Notice.IsEmpty()) ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
				})
			]
		];
	Bar = BarWidget;
	return BarWidget;
}
