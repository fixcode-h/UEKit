// Copyright Epic Games, Inc. All Rights Reserved.

#include "Probe/GameTestProbeOcclusion.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"

namespace
{
	/** 固定尺寸：命中测试的坐标换算要对得上，探针尺寸不能随内容漂。 */
	constexpr float ProbeWidth = 400.f;
	constexpr float ProbeHeight = 160.f;

	/** 左半留给目标按钮（Spacer 占位）、右半是遮罩；`pointX` 在 0.5 两侧给出不同答案。 */
	constexpr float GapFillFraction = 0.5f;
}

TSharedRef<SWidget> UGameTestProbeOcclusionWidget::RebuildWidget()
{
	if (WidgetTree != nullptr && WidgetTree->RootWidget == nullptr)
	{
		// 外层（widget.add 之后）会把这个 UserWidget 拉伸到整块视口（实测），所以尺寸要让**内部**的
		// 对齐说了算：根是全屏 Overlay，真正的内容是左上角一块固定尺寸的 ProbeArea。
		UOverlay* RootOverlay = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("ProbeRoot"));
		RootOverlay->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
		WidgetTree->RootWidget = RootOverlay;

		USizeBox* Area = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("ProbeArea"));
		Area->SetWidthOverride(ProbeWidth);
		Area->SetHeightOverride(ProbeHeight);
		if (UOverlaySlot* AreaSlot = RootOverlay->AddChildToOverlay(Area))
		{
			// 非 Fill 对齐 = 按 desired size 摆放（UOverlaySlot 的默认也是 Auto，写出来免得读的人猜）。
			AreaSlot->SetHorizontalAlignment(HAlign_Left);
			AreaSlot->SetVerticalAlignment(VAlign_Top);
		}

		UOverlay* AreaOverlay = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("ProbeOverlay"));
		AreaOverlay->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
		Area->AddChild(AreaOverlay);

		// 每一处 Fill 都显式写：UOverlaySlot 默认按内容尺寸，漏一处就变成"按钮只有文字那么大"。
		UButton* TargetButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("TargetButton"));
		UTextBlock* TargetLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("TargetLabel"));
		TargetLabel->SetText(FText::FromString(TEXT("目标按钮")));
		TargetButton->AddChild(TargetLabel);
		if (UOverlaySlot* ButtonSlot = AreaOverlay->AddChildToOverlay(TargetButton))
		{
			ButtonSlot->SetHorizontalAlignment(HAlign_Fill);
			ButtonSlot->SetVerticalAlignment(VAlign_Fill);
		}

		// 遮罩层：左半是 Spacer（显式尺寸，不参与命中）、右半是实心 Border（参与命中）。
		UHorizontalBox* MaskRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("MaskRow"));
		MaskRow->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
		if (UOverlaySlot* RowSlot = AreaOverlay->AddChildToOverlay(MaskRow))
		{
			RowSlot->SetHorizontalAlignment(HAlign_Fill);
			RowSlot->SetVerticalAlignment(VAlign_Fill);
		}

		USpacer* MaskGap = WidgetTree->ConstructWidget<USpacer>(USpacer::StaticClass(), TEXT("MaskGap"));
		MaskGap->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
		MaskGap->SetSize(FVector2D(ProbeWidth * GapFillFraction, ProbeHeight));
		if (UHorizontalBoxSlot* GapSlot = MaskRow->AddChildToHorizontalBox(MaskGap))
		{
			GapSlot->SetVerticalAlignment(VAlign_Fill);
		}

		UBorder* Mask = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Mask"));
		Mask->SetVisibility(ESlateVisibility::Visible);
		Mask->SetBrushColor(FLinearColor(0.95f, 0.45f, 0.1f, 0.75f));
		if (UHorizontalBoxSlot* MaskSlot = MaskRow->AddChildToHorizontalBox(Mask))
		{
			MaskSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			MaskSlot->SetVerticalAlignment(VAlign_Fill);
		}
	}

	return Super::RebuildWidget();
}
