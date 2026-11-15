// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class SWidget;
class UWidget;

/**
 * 只读命中测试：回答"这个点上最终会命中谁"（`probe` 动作的本体）。
 *
 * 走引擎自己的 hittest grid（`FSlateApplication::LocateWidgetInWindow`，与真实鼠标事件同一份数据），
 * **不**自算矩形重叠——矩形重叠不处理 HitTestInvisible、裁剪与层级次序，等于造第二套命中规则，
 * 与真实鼠标行为必然漂移。理由与被否决的替代见 docs/adr/ADR-0009-game-test-driver.md。
 *
 * 本类只查、只回报，不改任何状态。
 */
class GAMETEST_API GameTestHitTest
{
public:
	struct FResult
	{
		/** 从窗口根到最顶层命中控件的可读清单（Slate 类型名；能对上 UMG 控件的带上它）。 */
		TArray<FString> HitPath;

		/** 最顶层命中控件的可读描述（路径末项）。 */
		FString TopDescription;

		/** 最顶层命中控件的 Slate 指针（`expect` 判定用）。 */
		TSharedPtr<SWidget> TopSlateWidget;

		/** 命中路径里有没有目标的 Slate 控件——"未被遮挡"的判据。 */
		bool bTargetHit = false;

		/** 实际使用的控件内归一化点（已收进 0–1）。 */
		FVector2D Point = FVector2D(0.5, 0.5);
	};

	/**
	 * 对 `InWidget` 的 `InPoint`（控件内归一化坐标，0–1）做一次命中测试。
	 *
	 * 失败（没有 Slate 控件 / 几何无效 / 不在窗口里 / 查询拿不到路径）时返回 false 并填 `OutError`；
	 * 成功时 `OutResult` 里是命中路径与判定。
	 */
	static bool Probe(UWidget& InWidget, const FVector2D& InPoint, FResult& OutResult, FString& OutError);

	/** 同上，直接对 Slate 控件探（纯 Slate 目标、回放的「没被挡住」检查用）。 */
	static bool ProbeSlate(const TSharedRef<SWidget>& InSlateWidget, const FVector2D& InPoint, FResult& OutResult, FString& OutError);

	/**
	 * 把 Slate 控件反查成"属于哪个 UMG 控件"的可读描述——报告里的"遮挡者是谁"靠它。
	 * 引擎没有 UMG→Slate 的反向映射，只能在 UMG 对象里比对 `GetCachedWidget()` 的指针；查不到返回空串。
	 */
	static FString DescribeOwningWidget(const SWidget& InSlateWidget);
};
