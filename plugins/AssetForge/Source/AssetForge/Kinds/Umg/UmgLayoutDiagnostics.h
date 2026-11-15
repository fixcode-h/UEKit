// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 算法层：布局诊断。
 *
 * 存在的理由：写配置的 AI 看不见结果。它需要的不是一张图，而是「哪个控件和哪个重叠了、
 * 哪个跑到屏幕外了」这种可以直接据此改配置的结论——而这些全是算术，不需要渲染。
 *
 * 因此本文件不依赖 Slate、不依赖 RHI，在 `-nullrhi` 的 commandlet 与自动化测试里同样可用。
 *
 * 覆盖面限于 `CanvasPanel` 的直接子节点：只有它的插槽是「锚点 + 偏移」的绝对定位，
 * 矩形可以由配置独立算出来。其它面板（VerticalBox / Overlay / SizeBox…）的位置由面板
 * 自己的流式布局算法决定，不在这里猜——猜错的诊断比没有诊断更糟。
 */
namespace UmgLayoutDiagnostics
{
	/** 一条布局问题。 */
	struct ASSETFORGE_API FLayoutIssue
	{
		EForgeSeverity Severity = EForgeSeverity::Warning;
		FString NodePath;
		FString Message;
	};

	/**
	 * 在给定屏幕分辨率下检查布局问题。
	 *
	 * 检查项：矩形为负尺寸（配置错）、控件越出屏幕、同级控件大面积重叠、
	 * 以及 `bAutoSize` 导致尺寸无法估算因而无法检查的情况（会明说，不静默跳过）。
	 */
	ASSETFORGE_API void Analyze(const FForgeNode& InRoot, const FVector2D& InScreenSize, TArray<FLayoutIssue>& OutIssues);
}
