// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UWidget;

/**
 * `gt.resolve`：只解析选择器、不执行任何动作，告诉调用方「这个 target 现在能不能找到唯一的控件」。
 *
 * 面板编辑场景时用它把每一步的选择器标成找得到 / 找不到（带候选），Agent 写场景前也能先核对一遍，
 * 不用真跑一遍才知道第 7 步的选择器写错了。只读，不发输入，不改界面。只在游戏线程上用。
 */
class GAMETEST_API GameTestResolve
{
public:
	/** 一个要核对的选择器；Step / Field 记它从场景的哪一步、哪个字段来（直接给 targets 时为 -1 / 空）。 */
	struct FTarget
	{
		FString Target;
		int32 Step = -1;
		FString Field;
	};

	/** 每次最多核对多少个：一份场景几十步，几百个已经是很多份场景了。 */
	static constexpr int32 MaxTargets = 300;

	/** 步骤里写选择器的字段（`target`、拖动的 `toTarget`），按步骤顺序。 */
	static TArray<FTarget> CollectTargets(const TArray<TSharedPtr<FJsonObject>>& InSteps);

	/**
	 * 在当前游戏世界里逐个解析。没有游戏世界（PIE 没开）时返回 false 和原因——这时什么都核对不了，
	 * 不能把所有选择器都报成「找不到」。
	 */
	static bool Resolve(
		const TArray<FTarget>& InTargets,
		TMap<FString, TWeakObjectPtr<UWidget>>* InHandles,
		TSharedPtr<FJsonObject>& OutResult,
		FString& OutError);
};
