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
	// 颜色是线性空间（Slate 画的时候再转 sRGB），取值对着 UE 编辑器深色主题的面板和按钮调：
	// 底色约等于编辑器面板的 #1A1A1A，按钮平时透明、悬停才出底色，和编辑器工具栏按钮一个手感。
	const FLinearColor PanelColor(0.010f, 0.010f, 0.011f, 1.0f);
	const FLinearColor OutlineColor(1.0f, 1.0f, 1.0f, 0.09f);
	const FLinearColor TextColor(0.86f, 0.86f, 0.88f, 1.0f);
	const FLinearColor DimColor(0.36f, 0.37f, 0.40f, 1.0f);
	const FLinearColor RecordColor(0.90f, 0.12f, 0.10f, 1.0f);
	const FLinearColor MoveColor(0.16f, 0.48f, 0.95f, 1.0f);
	const FLinearColor NoticeColor(0.95f, 0.58f, 0.13f, 1.0f);

	/** 默认位置离视口底边的距离（Slate 单位，按视口的 DPI 缩放）：留出游戏底部常见的技能栏、聊天框边缘，又不至于飘到画面中间。 */
	constexpr double BottomGap = 56.0;

	/** 一排控件的统一高度：按钮、把手、分隔线都按它对齐，整条只有一行高。 */
	constexpr float RowHeight = 24.0f;

	/** 把手：2×3 个小点，告诉用户这里可以按住拖。不接点击，按下落到外层的工具栏底上处理。 */
	TSharedRef<SWidget> MakeGrip()
	{
		static const FSlateRoundedBoxBrush DotBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.30f), 1.5f);
		auto Dot = []()
		{
			return SNew(SBox).WidthOverride(3.0f).HeightOverride(3.0f) [ SNew(SBorder).BorderImage(&DotBrush).Padding(0.0f) ];
		};
		auto Column = [&Dot]()
		{
			return SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight() [ Dot() ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f) [ Dot() ]
				+ SVerticalBox::Slot().AutoHeight() [ Dot() ];
		};
		return SNew(SBox)
			.Visibility(EVisibility::HitTestInvisible)
			.HeightOverride(RowHeight)
			.VAlign(VAlign_Center)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth() [ Column() ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f, 0.0f, 0.0f) [ Column() ]
			];
	}

	/** 竖分隔线：把「状态」和「操作」分成两组，和编辑器工具栏的分组线一样细。 */
	TSharedRef<SWidget> MakeSeparator()
	{
		static const FSlateRoundedBoxBrush LineBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.10f), 0.0f);
		return SNew(SBox).WidthOverride(1.0f).HeightOverride(16.0f).Visibility(EVisibility::HitTestInvisible) [ SNew(SBorder).BorderImage(&LineBrush).Padding(0.0f) ];
	}

	/** 录制红点（录移动时变蓝）：一个实心小圆，慢慢明暗呼吸，一眼看出还在录。不用「●」字符，字体回退时大小和基线会跑。 */
	TSharedRef<SWidget> MakeRecordDot(TSharedRef<FGameTestRecordToolbar::FState> InState)
	{
		static const FSlateRoundedBoxBrush CircleBrush(FLinearColor::White, 4.0f);
		return SNew(SBox)
			.WidthOverride(8.0f)
			.HeightOverride(8.0f)
			.Visibility(EVisibility::HitTestInvisible)
			[
				SNew(SBorder)
				.BorderImage(&CircleBrush)
				.Padding(0.0f)
				.BorderBackgroundColor_Lambda([InState]()
				{
					const FLinearColor Base = InState->bMoving ? MoveColor : RecordColor;
					// 1.6 秒一个来回，亮度在 55%–100% 之间：动得看得见，又不至于晃眼。
					const float Pulse = 0.775f + 0.225f * FMath::Sin(float(FPlatformTime::Seconds()) * 3.9f);
					return FSlateColor(Base.CopyWithNewOpacity(Pulse));
				})
			];
	}

	/** 工具栏底：圆角深色面板加一圈很淡的描边。不依赖编辑器样式，独立游戏进程里也有。窗口不透明，所以底色也不透明。 */
	const FSlateBrush* BarBrush()
	{
		static const FSlateRoundedBoxBrush Brush(PanelColor, 6.0f, OutlineColor, 1.0f);
		return &Brush;
	}

	/** 普通按钮：平时透明只有字，悬停出浅底、按下再深一点，和编辑器工具栏按钮一样不抢眼。 */
	const FButtonStyle& ButtonStyle()
	{
		static const FButtonStyle Style = []
		{
			FButtonStyle Out;
			Out.SetNormal(FSlateRoundedBoxBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.0f), 4.0f));
			Out.SetHovered(FSlateRoundedBoxBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.08f), 4.0f));
			Out.SetPressed(FSlateRoundedBoxBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.14f), 4.0f));
			Out.SetNormalPadding(FMargin(8.0f, 0.0f));
			Out.SetPressedPadding(FMargin(8.0f, 1.0f, 8.0f, 0.0f));
			return Out;
		}();
		return Style;
	}

	/** 「停止」按钮：淡红底，放在最右边，和其他操作区分开，不容易误点又一眼找得到。 */
	const FButtonStyle& StopButtonStyle()
	{
		static const FButtonStyle Style = []
		{
			FButtonStyle Out = ButtonStyle();
			Out.SetNormal(FSlateRoundedBoxBrush(RecordColor.CopyWithNewOpacity(0.16f), 4.0f));
			Out.SetHovered(FSlateRoundedBoxBrush(RecordColor.CopyWithNewOpacity(0.28f), 4.0f));
			Out.SetPressed(FSlateRoundedBoxBrush(RecordColor.CopyWithNewOpacity(0.40f), 4.0f));
			return Out;
		}();
		return Style;
	}

	FSlateFontInfo Font(int32 InSize, bool bInBold = false)
	{
		return FCoreStyle::GetDefaultFontStyle(bInBold ? "Bold" : "Regular", InSize);
	}

	/** 按钮：点了走录制器的同一个入口（和快捷键、面板按钮一样），出错显示在工具栏的提示行。快捷键用暗色小字跟在后面。 */
	TSharedRef<SWidget> MakeButton(const FString& InLabel, const FString& InKeys, TSharedRef<FGameTestRecordToolbar::FState> InState, TFunction<void()> InOnClick, const FButtonStyle& InStyle, const FLinearColor& InLabelColor)
	{
		return SNew(SBox)
			.HeightOverride(RowHeight)
			[
				SNew(SButton)
				.ButtonStyle(&InStyle)
				// 按钮不拿键盘焦点：点完焦点还在游戏上，录按键、Alt+数字不受影响。
				.IsFocusable(false)
				.VAlign(VAlign_Center)
				.ToolTipText(InKeys.IsEmpty() ? FText::GetEmpty() : FText::FromString(FString::Printf(TEXT("%s（%s）"), *InLabel, *InKeys)))
				.OnClicked_Lambda([InOnClick]()
				{
					InOnClick();
					return FReply::Handled();
				})
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock).Text(FText::FromString(InLabel)).Font(Font(9)).ColorAndOpacity(InLabelColor)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(5.0f, 1.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(InKeys))
						.Font(Font(7))
						.ColorAndOpacity(DimColor)
						.Visibility_Lambda([InState, bHasKeys = !InKeys.IsEmpty()]() { return (bHasKeys && InState->bHotkeys) ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
					]
				]
			];
	}

	TSharedRef<SWidget> MakeButton(const FString& InLabel, const FString& InKeys, TSharedRef<FGameTestRecordToolbar::FState> InState, TFunction<void()> InOnClick)
	{
		return MakeButton(InLabel, InKeys, InState, MoveTemp(InOnClick), ButtonStyle(), TextColor);
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
	FitToContent();
	FollowViewport();
}

void FGameTestRecordToolbar::FitToContent()
{
	if (!Window.IsValid() || !Bar.IsValid())
	{
		return;
	}
	// 窗口大小自己按工具栏算：Slate 的自动尺寸窗口在一些机器上第一次建出来比内容高好几倍、之后也不缩回去，
	// 工具栏下面空一大片。这里每帧拿工具栏想要的大小（上一帧排版的结果）换成屏幕像素，差一个像素以上才改。
	const FVector2D Desired = FVector2D(Bar->GetDesiredSize());
	if (Desired.X <= 0.0 || Desired.Y <= 0.0)
	{
		return;
	}
	const double Scale = double(Window->GetDPIScaleFactor()) * double(FSlateApplication::Get().GetApplicationScale());
	const FVector2D Target(FMath::CeilToDouble(Desired.X * Scale), FMath::CeilToDouble(Desired.Y * Scale));
	const FVector2D Current = FVector2D(Window->GetSizeInScreen());
	if (FMath::Abs(Target.X - Current.X) > 1.0 || FMath::Abs(Target.Y - Current.Y) > 1.0)
	{
		Window->Resize(Target);
	}
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
		// 固定尺寸、由 FitToContent 按工具栏大小改：不用 Autosized，见 FitToContent 的说明。
		.SizingRule(ESizingRule::FixedSize)
		.ClientSize(FVector2D(640.0, 32.0))
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
	FitToContent();
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
		+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
		[ MakeButton(TEXT("录移动"), TEXT("Alt+1"), StateRef, [] { RunQuick(TEXT("start")); }) ]
		+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
		[ MakeButton(TEXT("记下位置"), TEXT("Alt+2"), StateRef, [] { RunQuick(TEXT("here")); }) ]
		+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
		[ MakeButton(TEXT("在这附近"), TEXT("Alt+3"), StateRef, [] { RunQuick(TEXT("near")); }) ]
		+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
		[ MakeButton(TEXT("断言"), TEXT("Alt+4"), StateRef, [] { RunQuick(TEXT("pick")); }) ]
		+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
		[ MakeButton(TEXT("检查点"), TEXT("Alt+5"), StateRef, [] { FString Error; FGameTestRecorder::Get().Mark(FString(), false, Error); }) ]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f)
		[ MakeSeparator() ]
		+ SHorizontalBox::Slot().AutoWidth()
		[ MakeButton(TEXT("停止"), FString(), StateRef, []
			{
				TSharedPtr<FJsonObject> Result;
				FString Error;
				FGameTestRecorder::Get().Stop(/*bInSave=*/true, Result, Error);
			}, StopButtonStyle(), FLinearColor(1.0f, 0.62f, 0.60f, 1.0f)) ];

	const TSharedRef<SWidget> Moving = SNew(SHorizontalBox)
		.Visibility_Lambda(MovingVisible)
		+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
		[ MakeButton(TEXT("结束移动"), TEXT("Alt+1"), StateRef, [] { RunQuick(TEXT("end")); }) ]
		+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
		[ MakeButton(TEXT("放弃"), TEXT("Alt+0"), StateRef, [] { RunQuick(TEXT("cancel")); }) ];

	const TSharedRef<SBorder> BarWidget = SNew(SBorder)
		.BorderImage(BarBrush())
		.Padding(FMargin(8.0f, 4.0f, 4.0f, 4.0f))
		.VAlign(VAlign_Top)
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
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)
				[
					MakeGrip()
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 7.0f, 0.0f)
				[
					MakeRecordDot(StateRef)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([StateRef]() { return FText::FromString(StateRef->Status); })
					.Font(Font(9, /*bInBold=*/true))
					.ColorAndOpacity(TextColor)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f)
				[
					MakeSeparator()
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center) [ Idle ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center) [ Moving ]
			]
			// 提示行：只在有提示时出现，和上面的状态文字左对齐（让过把手和红点）。
			+ SVerticalBox::Slot().AutoHeight().Padding(33.0f, 0.0f, 4.0f, 3.0f)
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
