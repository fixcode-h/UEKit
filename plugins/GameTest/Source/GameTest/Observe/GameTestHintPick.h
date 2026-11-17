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

	/**
	 * 选择器唯一命中了，但文字和录制时对不上：在**同一个容器**里找文字对得上的那个换过去。
	 *
	 * **为什么需要它**：录制器在没有名字可写时按位置写选择器（`#类名#序号`、`列表[序号]`、`容器/#类名#序号>…`）。
	 * 容器里的条目顺序会变（历史记录按最近使用排、列表重新排序），回放时同一个序号已经是别的按钮了，
	 * 按序号点就点错。录制时记下的 `hint.text` 能认出原来那个。
	 *
	 * 规则：
	 * 1. 解析到的控件文字和 `hint.text` 一样（或没录到文字）就不动：序号没变是绝大多数情况。
	 * 2. 从最靠近目标的那个按位置写的段开始，把序号换成同容器里的其他序号（只认此刻在屏上的），
	 *    文字和录制时一样的只有一个就换成它；有几个时按 `hint.rect` 挑最近的，挑不出就不换。
	 * 0. `hint.byIndex` 为 true（面板上选了「只按第几个」）或 `hint.key.cond` 有值（按唯一标识认）时不换。
	 * 3. 这一段的兄弟里一个都对不上，再往外一层按位置写的段找；都对不上就照旧按序号（文字可能本来就会变）。
	 *
	 * 换了返回 true，`InOutNode` 换成匹配的那个，`OutNote` 写怎么换的（进步骤结果的 `picked`）。
	 */
	GAMETEST_API bool Rematch(
		const TArray<TSharedPtr<FGameTestWidgetNode>>& InScreens,
		const FString& InTarget,
		const FJsonObject& InHint,
		TSharedPtr<FGameTestWidgetNode>& InOutNode,
		FString& OutNote);
}
