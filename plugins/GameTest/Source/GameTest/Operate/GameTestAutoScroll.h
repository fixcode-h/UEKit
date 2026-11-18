// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtrTemplates.h"

class SWidget;
class UWidget;

/**
 * 回放的 UI 动作在等目标可操作时，目标在列表或滚动框里没滚到的，先替用户滚过去。
 *
 * **为什么需要它**：录制时界面已经滚到了那里，回放时列表从头开始；虚拟化列表没滚到的条目根本没有控件，
 * 滚动框里没滚到的控件在可见区域外。以前要手写一步 `scroll`，录制的场景里一般没有这一步，回放就卡在「没找到」。
 * 只在回放里做（UI 动作 click / drag / wheel / type / key 等目标时，和「已达成就跳过」判断时）：`gt.resolve`、`snapshot` 只看不动。
 *
 * 三层，依次兜底：
 * 1. 选择器认得出是列表的第几个条目（`[3]`、`[itemKey=…]`），只是没生成：直接滚到那个条目。
 * 2. 找到了、但在滚动框可见范围外：滚过去。
 * 3. 整个找不到（按文字、路径认的条目没生成，没法知道是第几个）：**翻找**（1.7.1，用户 2026-11-18 定「没找到就一直滚，
 *    滚完还找不到就原地等待」）——把屏上所有显示着的 UserWidget（任何 UUserWidget 子类，不按名字认）里的列表、
 *    滚动框从头一页一页往后翻（选择器写的那个屏先翻），每翻一页等它生成再找；
 *    全都翻到底还没有就不再滚，原地等（到这一步的超时为止）。之后列表的条目数、滚动框的长度变了（数据晚到）就再翻一遍。
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

		/** 翻找：界面上的一个列表或滚动框翻到哪了。 */
		struct FScanBox
		{
			TWeakObjectPtr<UWidget> Widget;
			/** 开翻时的长度（列表条目数 / 滚动框可滚的长度）；变了说明数据晚到，重翻。 */
			float Extent = -1.f;
			/** 上一次翻到的位置，用来看出「翻不动了」（到底）。 */
			float LastOffset = -1.f;
			int32 Steps = 0;
			bool bStarted = false;
			bool bDone = false;
		};
		TArray<FScanBox> ScanBoxes;
		/** 从什么时候开始找不到（-1 = 现在找得到）：找不到先等一会儿再翻，界面刚打开时数据还没到。 */
		double MissingSince = -1.0;
		double LastScanAt = -1.0;
		/** 全都翻到底了：之后原地等，不再滚。 */
		bool bScanEnded = false;
	};

	/** 选择器刚因为「列表条目没生成」解析失败：把条目滚出来。滚了返回 true（调用方接着等下一帧）。 */
	bool ScrollToUngeneratedEntry(FState& InOutState);

	/** 目标在某个 ScrollBox 里、但不在它的可见范围：滚过去。滚了返回 true。 */
	bool ScrollIntoScrollBox(const TSharedPtr<SWidget>& InTarget, FState& InOutState);

	/**
	 * 目标整个找不到时翻找：翻屏上所有显示着的 UserWidget 里的列表和滚动框；`InTarget` 写了屏而且找得到时它的先翻。
	 * 这一帧翻了一页返回 true（调用方接着等，下一次再找）；还没到翻的时候、没东西可翻、全都翻到底了返回 false。
	 * 有 `InOutWhy` 时把「正在翻 / 翻完了原地等」接在后面给人看。
	 */
	bool ScanForMissing(const FString& InTarget, TMap<FString, TWeakObjectPtr<UWidget>>* InHandles, FState& InOutState, FString* InOutWhy = nullptr);

	/** 这一次找到了：翻找的计时清零（下次再找不到重新等一会儿再翻；翻到哪不清，接着翻）。 */
	void MarkFound(FState& InOutState);

	/** 屏名那一段（`WBP_Welfare:Root/…` → `WBP_Welfare`）；句柄、纯 Slate、`*`、没写屏名的返回空。 */
	FString ScreenOf(const FString& InTarget);
}
