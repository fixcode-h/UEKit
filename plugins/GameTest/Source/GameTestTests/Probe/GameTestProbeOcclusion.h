// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Blueprint/UserWidget.h"
#include "GameTestProbeOcclusion.generated.h"

/**
 * 遮挡探针：一个"半边被盖住"的靶子，用来验 `probe` 命中测试动作。
 *
 * 结构：目标 `UButton`（`TargetButton`）填满整块，右半被一块纯色 `UBorder`（`Mask`）盖住——
 * 于是同一个控件同时提供"命中目标"（左半）与"命中遮罩"（右半）两个区域，
 * `pointX` 参数本身也被验到；不需要开关、不需要新动作、也不需要任何资产。
 *
 * 纯 C++（没有 WBP）：`widget.add class=GameTestProbeOcclusionWidget` 直接生成。
 * 布局里的每一处命中可见性都是**显式**设的——面板与 Spacer 不参与命中、遮罩参与，
 * 否则"左半命中按钮"会退化成"命中 Spacer"。
 */
UCLASS()
class GAMETESTTESTS_API UGameTestProbeOcclusionWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget interface
	virtual TSharedRef<SWidget> RebuildWidget() override;
	//~ End UUserWidget interface
};
