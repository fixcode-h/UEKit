// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UInputAction;
class UWorld;

/**
 * 输入资产的持续注入（走 Enhanced Input）。光标移动在主模块的 `GameTestInput`。
 *
 * **注入为什么走 Enhanced Input 的注入套件**：引擎自带
 * `IEnhancedInputSubsystemInterface::StartContinuousInputInjectionForAction`，它注册一个
 * "每 tick 重发"的注入项，值走完整的 modifier 与 trigger 链——引擎注释原文
 * "injects the given input every tick until it is stopped"。这与"直接调载具的输入处理函数"有本质区别：
 * 后者绕过 IMC/Action/trigger 层，测的是逻辑而不是"玩家操作这条路"。本插件不依赖游戏模块，
 * 注入按**资产路径**解析，正好也绕开了对游戏类的引用。
 *
 * **被否决**：`APlayerController::InputKey`（合成 FKey）——它绕过 IMC/Action 层，等于自建第二套
 * 按键→动作映射，且拿不到 trigger 语义。
 */
class GameTestInjection
{
public:
	/**
	 * 资产路径/裸名 → 已加载的 `UInputAction`。
	 *
	 * 写法与 `GameTestOps::ResolveWidgetClass` 一致：`/Game/...` 走 `StaticLoadObject`，
	 * 裸名在已加载对象里找；失败时给候选清单（AI 一次就能改对）。
	 */
	static UInputAction* ResolveInputAction(const FString& InActionPath, FString& OutError);

	/**
	 * 开始（或更新）对某个输入动作的持续注入。
	 *
	 * 值用 `InValue` 填满该动作 ValueType 的**各个分量**：Axis1D 用 X、Axis2D 用 X/Y、
	 * Axis3D 用 XYZ。按类型填，换了动作类型也不会静默失效。
	 *
	 * **不传 modifier/trigger**（引擎的 `FindOrAdd` 语义使重复注入以最后一值为准）：映射里没配
	 * modifier 时注入路径与真实按键逐帧同路；配了的话会和真实按键有差异，报告里看得出来。
	 *
	 * 参数用 `const` 是因为引擎侧接口本身就是 `const UInputAction*`，而收尾清理要在只能拿到
	 * 弱引用（`TWeakObjectPtr<const UInputAction>`）时调它。
	 */
	static bool StartInjection(UWorld& InWorld, const UInputAction& InAction, double InValue, FString& OutMessage);

	/** 停止注入；对没在注入的动作返回 false 并说明（不静默成功）。 */
	static bool StopInjection(UWorld& InWorld, const UInputAction& InAction, FString& OutMessage);

	/** 当前是否在给该动作注入（`ping`/health 报活跃数用）。 */
	static bool IsInjecting(UWorld& InWorld, const UInputAction& InAction);
};
