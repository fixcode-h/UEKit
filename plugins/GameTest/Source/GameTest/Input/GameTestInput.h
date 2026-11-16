// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FKey;
class SWidget;
class UWorld;

/**
 * 光标移动。输入资产的持续注入在可选模块 GameTestEnhancedInput 里（`GameTestInjection`）。
 *
 * **光标为什么单独做**：不少游戏的瞄准链路直读鼠标位置（`APlayerController::GetMousePosition`），它
 * 不经 Enhanced Input，注入动作值替代不了。所以 `MoveCursor` 直接移光标（`FSlateApplication::SetCursorPos`）。
 * 这和"用坐标点击 UMG"不是一回事：点控件有更好的路（控件的公开接口），GameTest 不走坐标；
 * 这里驱动的是**游戏逻辑**（没有替代，除非改游戏代码加测试钩子，那就不是真实模拟了）。
 * 代价是它移动**真实光标**——对"真实模拟"是优点，对"无感"是代价。
 */
class GAMETEST_API GameTestInput
{
public:
	/**
	 * 把光标移到**视口内归一化坐标**（0–1）。
	 *
	 * 换算链：视口归一化 → 视口绝对矩形（屏幕坐标）→ `SetCursorPos`。归一化坐标是与分辨率无关的
	 * 表达，AI 写场景时不必知道 PIE 窗口开在屏幕哪里。
	 *
	 * @param OutScreenPosition 实际使用的屏幕坐标（返回体里给出来，便于对账）。
	 */
	static bool MoveCursor(UWorld& InWorld, const FVector2D& InNormalizedPoint, FVector2D& OutScreenPosition, FString& OutMessage);

	/**
	 * 按下或松开一个键（键盘键或鼠标键），走 Slate 的输入入口：
	 * `FSlateApplication::ProcessKeyDownEvent` / `ProcessMouseButtonDownEvent`。
	 *
	 * 这条路和真实按键一样：Slate → 游戏视口 → PlayerController → EnhancedInput 的映射与 trigger，
	 * 所以不需要知道项目的输入资产，录下来的按键能原样回放。键盘事件发之前把焦点交给游戏视口，
	 * 否则会落到编辑器的面板上。鼠标键在光标当前位置按下（先用 `input.cursor` 摆好位置）。
	 *
	 * 按下而没松开的键记在账上，run 收尾时 `ReleaseHeldKeys` 全部松开，不留卡住的键。
	 */
	static bool SendKey(const FKey& InKey, bool bInDown, FString& OutMessage);

	/**
	 * 同 `SendKey`，但键盘事件发给 `InFocus`（先把用户 0 的焦点交给它）：回放「在输入框里按回车 / 退格」用。
	 * `InFocus` 为空时和 `SendKey` 一样交给游戏视口。
	 */
	static bool SendKeyTo(const FKey& InKey, bool bInDown, const TSharedPtr<SWidget>& InFocus, FString& OutMessage);

	/**
	 * 把光标移到**屏幕绝对坐标**并发一次鼠标移动：Slate 据此更新悬停（按钮松开时要「还悬停在上面」才算点击）。
	 * 回放点控件用它：控件的位置每次都按选择器现找，不用录下来的坐标。
	 */
	static bool MovePointerTo(const FVector2D& InScreenPosition, FString& OutMessage);

	/**
	 * 虚拟鼠标点击：在屏幕坐标上发一组移动 / 按下 / 松开的 Slate 事件（左键），**不移动系统光标**。
	 * 三个事件在同一帧里发完：隔帧的话，Slate 每帧按真实光标位置合成的移动会把悬停改回去，按钮松开时就不算点击。
	 */
	static bool SendVirtualClick(const FVector2D& InScreenPosition, FString& OutMessage);

	/** 在光标当前位置滚一次滚轮（正数向上）。 */
	static bool SendWheel(float InDelta, FString& OutMessage);

	/** 逐字发字符事件（`KeyChar`），和真实打字同一条路；发给当前有键盘焦点的控件。 */
	static bool SendCharacters(const FString& InText, FString& OutMessage);

	/** Ctrl+A 全选（输入框替换文本前用）。 */
	static bool SendSelectAll(FString& OutMessage);

	/**
	 * 虚拟光标开关（每次 run 开始时按点击方式设）：开着时 MoveCursor / MovePointerTo 不移系统光标，
	 * 只把位置记下来带进之后的鼠标事件（按键、滚轮），回放不占用人手里的鼠标。
	 * 代价：游戏里直接读系统光标位置的逻辑（按鼠标位置瞄准）看不到虚拟光标，这种场景用 clickVia=pointer。
	 */
	static void SetVirtualCursor(bool bInVirtual);
	static bool IsVirtualCursor();

	/** 松开本次 run 里按下还没松开的键（run 收尾时调）。 */
	static void ReleaseHeldKeys();

	/** 正在发合成输入：录制器据此不把回放出来的按键再录一遍。 */
	static bool IsSendingSynthetic();

	/** 回放按住还没松开的键（「LeftAlt、W」），没有时返回空串；点击的诊断日志用，说明这次点击带着哪些修饰键。 */
	static FString DescribeHeldKeys();
};
