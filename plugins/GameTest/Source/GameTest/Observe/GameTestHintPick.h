// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
struct FGameTestWidgetNode;

/**
 * 选择器命中多个时，按录制写下的线索（`hint`）挑回录的那一个。
 *
 * **为什么需要它**：同一个 WBP 的几个实例直接加在视口上（或挂在快照认不出的地方）时，录制器写不出唯一的路径，
 * 只能写到 `WBP名:路径`。这时选择器照规矩报歧义、不取第一个；但录制当时点的是哪一个是有据可查的——
 * 位置（`rect`）、文字（`text`）。只在命中多个、并且步骤带着录制线索时才挑，手写的选择器照旧报歧义。
 *
 * 规则（依次缩小，剩一个就停）：
 * 1. 只留此刻真在屏上的：在游戏视口里、整条父链可见、有尺寸（对象池里藏着的、折叠的不算）。
 * 2. 有 `text` 时留文字完全一样的（一个都对不上就不按文字筛：换了语言之类）。
 * 3. 有 `rect` 时按位置和大小挑最近的；离得太远、或最近的两个差不多近，就不挑，报错写清楚。
 */
namespace GameTestHintPick
{
	/** 步骤里的录制线索（`hint` 对象）；没有时返回 nullptr。 */
	GAMETEST_API const FJsonObject* GetHint(const TSharedPtr<FJsonObject>& InStepParams);

	/**
	 * 在候选里挑一个。挑中返回 true，`OutNote` 写怎么挑的（进步骤结果）；
	 * 挑不出返回 false，`OutWhy` 写还差什么（回放会接着等，超时时拼进失败原因）。
	 */
	GAMETEST_API bool Pick(
		const TArray<TSharedPtr<FGameTestWidgetNode>>& InCandidates,
		const FJsonObject& InHint,
		TSharedPtr<FGameTestWidgetNode>& OutNode,
		FString& OutNote,
		FString& OutWhy);
}
