// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class SWidget;

/**
 * 不动真实鼠标的点击（click 的默认方式，1.2.0-beta.7 起）：
 *
 * 1. 从目标往上（不越过它所在的 WBP）找最近的按钮 / 复选框 / 菜单锚点，直接调对象的委托：
 *    按钮依次广播 OnPressed → OnClicked → OnReleased，蓝图、Lua 绑在哪一个上都能收到。
 *    目标本身是一个 WBP（列表条目之类）时，在它里面找唯一一个可见的按钮。
 * 2. 找不到这类控件（WBP 自己重写了 OnMouseButtonDown、UImage 绑的事件）时退到「虚拟鼠标」：
 *    在控件的位置发一组移动 / 按下 / 松开的 Slate 鼠标事件，事件带着坐标，不移动系统光标。
 *
 * 真实鼠标的点击（`via: "pointer"`）照旧保留：要验证遮挡、悬停这类只有真鼠标才有的行为时用。
 */
class GAMETEST_API GameTestCodeClick
{
public:
	/**
	 * 点 `InTarget`（已确认可见、可用、有尺寸）。`InScreen` 是虚拟鼠标要用的屏幕坐标（控件内 at 换算出来的）。
	 * 结果写进 `OutPayload`：`via`（code / virtualPointer）、`clicked`（实际被调的控件）。
	 */
	static bool Click(const TSharedPtr<SWidget>& InTarget, const FVector2D& InScreen, FJsonObject& OutPayload, FString& OutMessage);
};
