// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/GameTestTypes.h"
#include "Operate/GameTestAutoScroll.h"

class FJsonObject;
class FJsonValue;
class UWidget;

/**
 * 已达成就跳过（1.6.0，用户 2026-11-18 定稿）：步骤的 `skipIf`。
 *
 * **为什么要它**：录一步的目的常常是让界面变成某个样子（勾上 ResetPoint、领取后按钮变「已领取」）。
 * 回放时界面已经是那个样子了，再点一次反而会把勾取消、或卡在一个点不动的按钮上。
 *
 * ```json
 * { "action": "click", "target": "WBP_Settings:Root/RowResetPoint>BtnToggle",
 *   "skipIf": { "target": "WBP_Settings:Root/RowResetPoint>ChkBox", "checked": true, "also": 0, "verify": true } }
 * ```
 *
 * - 条件和 `assert` 共用（GameTestCondition）；`target` 省略 = 这一步自己的 target。
 * - 回放时先判断再动手（FGameTestSkipGate）：条件成立并稳住 `settleMs` 就跳过，记 `already`（算通过）；
 *   不成立并稳住 `settleMs`、目标也出来了才照常做。多等这一小会儿是怕界面数据还没刷上来（按钮先显示「领取」，回包后才变「已领取」）。
 * - 反面条件（`exists: false` / `visible: false`）只在条件看的屏已经在屏上时才算数：找不到既可能是领过了，也可能是界面还没打开。
 * - `also`：跳过时连带跳过后面几步（领取跳过了，奖励弹窗不会出来，点弹窗「确定」那一步也不该等）。
 * - `verify`：真做了以后条件要在 `verifyTimeoutMs` 里成立，不然这一步失败（点了没反应）。
 *
 * 只在游戏线程上用。
 */
namespace GameTestSkipIf
{
	constexpr int32 DefaultSettleMs = 300;
	constexpr int32 DefaultVerifyTimeoutMs = 5000;
	constexpr int32 MaxSettleMs = 10000;
	constexpr int32 MaxVerifyTimeoutMs = 120000;
	constexpr int32 MaxAlso = 20;

	/** 步骤的 `skipIf` 对象；没写、写成 null 返回空。 */
	GAMETEST_API TSharedPtr<FJsonObject> Get(const TSharedPtr<FJsonObject>& InStepParams);

	/** 检查步骤里的 `skipIf` 写得对不对（条件、配置字段、要看的控件）。 */
	GAMETEST_API bool ValidateStep(const FJsonObject& InStepJson, FString& OutError);

	/**
	 * 运行配置、场景顶层的 `skipIf` 段盖到选项上：`false` 整个关掉，`true` 打开，
	 * 对象时逐项改（`enabled`、`settleMs`、`verify`、`verifyTimeoutMs`），写错报哪一项。
	 */
	GAMETEST_API bool ApplyOptions(const FJsonValue& InValue, FGameTestSkipOptions& InOutOptions, FString& OutError);

	/** 这一步要不要做完核对、核对等多久（步骤字段盖在选项上）。 */
	GAMETEST_API bool ShouldVerify(const FJsonObject& InSkipIf, const FGameTestSkipOptions& InOptions);
	GAMETEST_API int32 VerifyTimeoutMs(const FJsonObject& InSkipIf, const FGameTestSkipOptions& InOptions);

	/** 连带跳过后面几步（0–MaxAlso）。 */
	GAMETEST_API int32 Also(const FJsonObject& InSkipIf);

	/**
	 * 此刻条件成立吗（做完核对每帧调）。`OutActual` 写实际值，失败消息用。
	 * 条件看的控件不在屏上算不成立；反面条件所在的屏不在屏上也算不成立（核对要的是「确实达成了」）。
	 */
	GAMETEST_API bool Holds(const FJsonObject& InSkipIf, const FGameTestStep& InStep, TMap<FString, TWeakObjectPtr<UWidget>>* InHandles, FString& OutActual);
}

/** 跳过判断每帧的结论。 */
enum class EGameTestSkipGate : uint8
{
	/** 还在等：条件看的控件没出来、或刚成立 / 刚不成立还没稳住。 */
	Wait,
	/** 已达成：跳过这一步（和 also 连带的几步）。 */
	Skip,
	/** 没达成：照常做。 */
	Proceed,
};

/**
 * 一步开始前的跳过判断（执行器每帧调 Tick）。见 GameTestSkipIf 的说明。
 */
class GAMETEST_API FGameTestSkipGate
{
public:
	FGameTestSkipGate(const FGameTestStep& InStep, const TSharedPtr<FJsonObject>& InSkipIf,
		TMap<FString, TWeakObjectPtr<UWidget>>* InHandles, const FGameTestSkipOptions& InOptions, double InNow);

	EGameTestSkipGate Tick(double InNow);

	/** Skip 时：给人看的一句（「已达成，跳过：checked=true（checked=true）」）；Wait 时：此刻卡在哪（超时消息用）。 */
	const FString& GetMessage() const
	{
		return Message;
	}

	/** Skip 时写进步骤结果 `payload.skip`：`{cond, target, actual, waitedMs, also?}`。 */
	TSharedPtr<FJsonObject> MakeSkipPayload(double InNow) const;

private:
	FGameTestStep Step;
	TSharedPtr<FJsonObject> SkipIf;
	TMap<FString, TWeakObjectPtr<UWidget>>* Handles = nullptr;
	FString CondTarget;
	double StartedAt = 0.0;
	double SettleSeconds = 0.3;
	double HoldSince = -1.0;
	double NotSince = -1.0;
	double LastEvalAt = -1.0;
	EGameTestSkipGate LastResult = EGameTestSkipGate::Wait;
	FString Message;
	FString Actual;
	/** 看的控件在列表里没滚到时替用户滚、翻找（1.7.1）；滚过什么写进跳过结果的 `scrolled`。 */
	GameTestAutoScroll::FState Scroll;
};
