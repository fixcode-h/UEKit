// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UWidget;

/**
 * `gt.resolve targets / scenario`：只解析步骤的目标（部件写法 `{obj, part}`、引用、`@句柄`）、不执行任何动作，
 * 告诉调用方「这个目标现在能不能找到唯一的控件」。2.0.0 起不认选择器（报 selector_removed）。
 *
 * 面板编辑场景时用它把每一步的目标标成找得到 / 找不到，Agent 写场景前也能先核对一遍，
 * 不用真跑一遍才知道第 7 步写错了。只读，不发输入，不改界面。只在游戏线程上用。
 */
class GAMETEST_API GameTestResolve
{
public:
	/** 一个要核对的目标；Step / Field 记它从场景的哪一步、哪个字段来（直接给 targets 时为 -1 / 空）。 */
	struct FTarget
	{
		/** 内部写法（GameTestHandlers::TargetText 的结果）；Error 非空时为空。 */
		FString Target;
		/** 写法本身就不对（旧选择器、类型不对）：不解析，原样报。 */
		FString Error;
		int32 Step = -1;
		FString Field;
	};

	/** 每次最多核对多少个：一份场景几十步，几百个已经是很多份场景了。 */
	static constexpr int32 MaxTargets = 300;

	/** 步骤里写目标的字段（`target`、拖动的 `toTarget`、滚动的 `widget`），按步骤顺序。 */
	static TArray<FTarget> CollectTargets(const TArray<TSharedPtr<FJsonObject>>& InSteps);

	/** `targets` 数组里的一项（字符串或对象）。 */
	static FTarget FromValue(const TSharedPtr<FJsonValue>& InValue);

	/**
	 * 在当前游戏世界里逐个解析。`InObjects` 是场景的 `objects` 段（命名引用要它，可为空）。
	 * 没有游戏世界（PIE 没开）时返回 false 和原因——这时什么都核对不了，不能把所有目标都报成「找不到」。
	 */
	static bool Resolve(const TArray<FTarget>& InTargets, TMap<FString, TWeakObjectPtr<UWidget>>* InHandles, const FJsonObject* InObjects, TSharedPtr<FJsonObject>& OutResult, FString& OutError);
};
