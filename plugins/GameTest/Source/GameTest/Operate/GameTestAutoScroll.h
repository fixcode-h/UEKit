// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class SWidget;

/**
 * 回放的 UI 动作在等目标可操作时，目标在列表或滚动框里没滚到的，先替用户滚过去。
 *
 * **为什么需要它**：录制时界面已经滚到了那里，回放时列表从头开始；虚拟化列表没滚到的条目根本没有控件，
 * 滚动框里没滚到的控件在可见区域外。以前要手写一步 `scroll`，录制的场景里一般没有这一步，回放就卡在「没找到」。
 * 只在回放的 UI 动作里做（click / drag / wheel / type / key）：`gt.resolve`、`snapshot` 只看不动。
 */
namespace GameTestAutoScroll
{
	/** 一步里自动滚动的记账：同一个列表的同一个序号只滚一次、滚动框最多滚几次，不来回拖。 */
	struct FState
	{
		TSet<FString> ScrolledEntries;
		int32 ScrollBoxAttempts = 0;
		/** 滚过什么（进步骤结果的 `scrolled`）。 */
		FString Note;
	};

	/** 选择器刚因为「列表条目没生成」解析失败：把条目滚出来。滚了返回 true（调用方接着等下一帧）。 */
	bool ScrollToUngeneratedEntry(FState& InOutState);

	/** 目标在某个 ScrollBox 里、但不在它的可见范围：滚过去。滚了返回 true。 */
	bool ScrollIntoScrollBox(const TSharedPtr<SWidget>& InTarget, FState& InOutState);
}
