// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * 录制时的文字（`hint.text`）和此刻控件文字的比较，分三档。
 *
 * **为什么不只比原文**：游戏里按钮上的字常带会变的部分——数量「领取(3)」、倒计时「剩余 02:15」、
 * 富文本标签 `<red>NEW</>`。原文一比就对不上，回放只能退回按位置点。三档依次放宽，先中的为准：
 * 1. Exact：原文相等。
 * 2. Normalized：去掉富文本标签、数字、空白和标点后相等（数量、倒计时变了还认得出）。
 * 3. Contains：归一后一方包含另一方，短的至少 2 个字、且不短于长的 40%（「商店」和「商店NEW」）。
 * 换了语言时三档都对不上，调用方退回按位置。
 */
namespace GameTestTextMatch
{
	enum class ETier : uint8
	{
		None,
		Contains,
		Normalized,
		Exact,
	};

	/** 归一：去富文本标签、数字（含全角）、空白、中英文标点，ASCII 字母转小写。 */
	GAMETEST_API FString Normalize(const FString& InText);

	/** 两段文字按哪一档对得上（`InWant` 为空时 None）。 */
	GAMETEST_API ETier Compare(const FString& InWant, const FString& InNow);

	/**
	 * 在若干文字里找对得上的，只留最高的那一档：返回这一档，`OutIndices` 是这一档里的下标。
	 * 一个都对不上返回 None、`OutIndices` 为空。
	 */
	GAMETEST_API ETier FilterBest(const FString& InWant, const TArray<FString>& InTexts, TArray<int32>& OutIndices);

	/** 给人看的档名（进步骤结果的 picked）。 */
	GAMETEST_API const TCHAR* Describe(ETier InTier);
}
