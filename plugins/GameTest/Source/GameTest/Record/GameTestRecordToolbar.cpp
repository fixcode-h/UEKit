// Copyright Epic Games, Inc. All Rights Reserved.

#include "Record/GameTestRecordToolbar.h"

#include "Core/GameTestWorld.h"
#include "Record/GameTestRecorder.h"
#include "GameTest.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Dom/JsonObject.h"
#include "Engine/GameViewportClient.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace GameTestRecordToolbarDetail
{
	/** 盖在游戏上面：比游戏自己的界面（通常 0–100）高，也比 run 时的高亮框高。 */
	constexpr int32 ZOrder = 10050;

	const FLinearColor TextColor(0.92f, 0.92f, 0.94f, 1.0f);
	const FLinearColor DimColor(0.62f, 0.64f, 0.68f, 1.0f);
	const FLinearColor RecordColor(0.94f, 0.33f, 0.31f, 1.0f);
	const FLinearColor MoveColor(0.35f, 0.68f, 0.98f, 1.0f);
	const FLinearColor NoticeColor(0.98f, 0.76f, 0.35f, 1.0f);

	/** 默认位置离视口底边的距离：留出游戏底部常见的技能栏、聊天框边缘，又不至于飘到画面中间。 */
	constexpr double BottomGap = 56.0;
	/** 拖动时离视口四边至少留这么多，工具栏不会被拖出画面找不回来。 */
	constexpr double EdgeGap = 4.0;

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

	/** 半透明深色圆角底，和面板深色主题的浮层接近；不依赖编辑器样式，游戏进程里也有。 */
	const FSlateBrush* BarBrush()
	{
		static const FSlateRoundedBoxBrush Brush(FLinearColor(0.07f, 0.07f, 0.09f, 0.88f), 8.0f, FLinearColor(1.0f, 1.0f, 1.0f, 0.10f), 1.0f);
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
	if (Current == Viewport.Get() && Root.IsValid())
	{
		return;
	}
	Hide();
	if (Current == nullptr)
	{
		return;
	}
	Root = Build();
	Current->AddViewportWidgetContent(Root.ToSharedRef(), GameTestRecordToolbarDetail::ZOrder);
	Viewport = Current;
}

void FGameTestRecordToolbar::Hide()
{
	if (UGameViewportClient* Current = Viewport.Get())
	{
		if (Root.IsValid())
		{
			Current->RemoveViewportWidgetContent(Root.ToSharedRef());
		}
	}
	Root.Reset();
	Bar.Reset();
	Viewport.Reset();
	bDragging = false;
}

bool FGameTestRecordToolbar::Contains(const FVector2D& InScreenPosition) const
{
	if (!Bar.IsValid() || Bar->GetVisibility() == EVisibility::Collapsed)
	{
		return false;
	}
	const FGeometry& Geometry = Bar->GetCachedGeometry();
	const FVector2D Local = FVector2D(Geometry.AbsoluteToLocal(InScreenPosition));
	const FVector2D Size = FVector2D(Geometry.GetLocalSize());
	return Size.X > 0.0 && Local.X >= 0.0 && Local.Y >= 0.0 && Local.X <= Size.X && Local.Y <= Size.Y;
}

FVector2D FGameTestRecordToolbar::ToRootLocal(const FVector2D& InScreenPosition) const
{
	if (!Root.IsValid())
	{
		return InScreenPosition;
	}
	return FVector2D(Root->GetCachedGeometry().AbsoluteToLocal(InScreenPosition));
}

FVector2D FGameTestRecordToolbar::PlacedPosition() const
{
	using namespace GameTestRecordToolbarDetail;
	if (!Root.IsValid() || !Bar.IsValid())
	{
		return FVector2D::ZeroVector;
	}
	const FVector2D Area = FVector2D(Root->GetCachedGeometry().GetLocalSize());
	const FVector2D Size = FVector2D(Bar->GetDesiredSize());
	const FVector2D Want = bPlaced ? Position : FVector2D((Area.X - Size.X) * 0.5, Area.Y - Size.Y - BottomGap);
	// 每帧按当前视口大小夹一次：窗口缩小、工具栏变宽（切到「结束移动」）时都不会出画面。视口比工具栏还窄时贴左上。
	const double MaxX = FMath::Max(EdgeGap, Area.X - Size.X - EdgeGap);
	const double MaxY = FMath::Max(EdgeGap, Area.Y - Size.Y - EdgeGap);
	return FVector2D(FMath::Clamp(Want.X, EdgeGap, MaxX), FMath::Clamp(Want.Y, EdgeGap, MaxY));
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
		// 视口第一次排版前拿不到大小，先藏一帧，免得在左上角闪一下再跳到中下方。
		.Visibility_Lambda([WeakSelf]()
		{
			const TSharedPtr<FGameTestRecordToolbar> This = WeakSelf.Pin();
			const bool bLaidOut = This.IsValid() && This->Root.IsValid() && This->Root->GetCachedGeometry().GetLocalSize().X > 0.0f;
			return bLaidOut ? EVisibility::Visible : EVisibility::Hidden;
		})
		.OnMouseButtonDown_Lambda([WeakSelf](const FGeometry&, const FPointerEvent& InEvent)
		{
			// 按在把手、状态文字或工具栏空白处开始拖：按钮自己接住点击，到不了这里。
			// 录制器那边看到按下点在工具栏上就不录，配对的松开也跳过，所以拖动不会变成步骤。
			const TSharedPtr<FGameTestRecordToolbar> This = WeakSelf.Pin();
			if (!This.IsValid() || !This->Bar.IsValid() || InEvent.GetEffectingButton() != EKeys::LeftMouseButton)
			{
				return FReply::Unhandled();
			}
			This->Position = This->PlacedPosition();
			This->bPlaced = true;
			This->GrabOffset = This->ToRootLocal(FVector2D(InEvent.GetScreenSpacePosition())) - This->Position;
			This->bDragging = true;
			return FReply::Handled().CaptureMouse(This->Bar.ToSharedRef());
		})
		.OnMouseMove_Lambda([WeakSelf](const FGeometry&, const FPointerEvent& InEvent)
		{
			const TSharedPtr<FGameTestRecordToolbar> This = WeakSelf.Pin();
			if (!This.IsValid() || !This->bDragging)
			{
				return FReply::Unhandled();
			}
			// 不在这里夹边界：PlacedPosition 每帧夹，拖回来时工具栏还跟着鼠标走。
			This->Position = This->ToRootLocal(FVector2D(InEvent.GetScreenSpacePosition())) - This->GrabOffset;
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
			// 松手时把夹过的位置存回去，下次拖从看得见的地方开始。
			This->Position = This->PlacedPosition();
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

	// 外层铺满视口但不挡游戏的输入：只有工具栏本身接点击。工具栏按左上角定位，位置每帧从 PlacedPosition 取。
	return SNew(SOverlay)
		.Visibility(EVisibility::SelfHitTestInvisible)
		+ SOverlay::Slot()
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Top)
		.Padding(TAttribute<FMargin>::CreateLambda([WeakSelf]()
		{
			const TSharedPtr<FGameTestRecordToolbar> This = WeakSelf.Pin();
			const FVector2D At = This.IsValid() ? This->PlacedPosition() : FVector2D::ZeroVector;
			return FMargin(float(At.X), float(At.Y), 0.0f, 0.0f);
		}))
		[
			BarWidget
		];
}
