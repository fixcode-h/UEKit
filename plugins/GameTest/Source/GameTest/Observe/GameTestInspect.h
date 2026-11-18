// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * `gt.inspect` / `GameTest.Inspect`：看某个点下面到底是哪个控件、选择器该怎么写。
 *
 * 录出来的步骤不对（点到了别的控件、写出了坐标而不是控件）时用它查：返回命中路径（Slate 类型和对应的 UMG 控件）、
 * 最深的 UMG 控件写出的选择器和核对结果、最近一次鼠标按下是被谁接住的。只读，不发任何输入。只在游戏线程上用。
 */
class GAMETEST_API GameTestInspect
{
public:
	/**
	 * `InNormalizedPoint` 是视口归一化坐标；不给时用当前光标位置。
	 * 返回：`supported`、`handlerSource`、`point`、`path`（从外到内，最多 30 项）、`target`、`verified`、`hint`、`lastHandler`；
	 * 1.10.0 起还有 `widgetClass`、`kind`（button / check / text / other，面板一键分路按它给分法）、`rect`（控件在视口里的归一化 [x, y, w, h]）。
	 * `bInPreferInteractive`（抓控件用）：点到按钮、勾选框里面的文字或图片时，往上最多 4 层换成那个按钮 / 勾选框。
	 */
	static bool Inspect(const TOptional<FVector2D>& InNormalizedPoint, TSharedPtr<FJsonObject>& OutResult, FString& OutError, bool bInPreferInteractive = false);
};
