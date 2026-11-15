// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FGameTestWidgetNode;

/**
 * 选择器：把一段文本解析成"树里的哪个控件"。
 *
 * 语法（`target` 字段）：
 * - `@句柄`            —— 引用之前 `find` 捕获的控件（绕过重解析）
 * - `屏名:路径`        —— 先按屏名（类名或控件名，忽略大小写）挑屏，再走路径
 * - `路径`             —— 不写屏名：在所有屏里找，多屏命中即歧义
 * - 路径段：`名字` / `#类名` / `#类名#序号` / `text=子串` / `*`（任一子节点）/ `..`（任一后代）
 *
 * 解析失败一律**当作错误**并带上候选清单——不做"取第一个匹配"这种静默行为。
 * 整棵树是唯一的搜索空间：与快照共用同一份模型，AI 看到的与点到的是同一个控件。
 */
class GAMETEST_API GameTestSelector
{
public:
	struct FParsed
	{
		bool bHandle = false;
		FString Handle;
		bool bAnyScreen = false;
		FString Screen;
		TArray<FString> Segments;
	};

	static bool Parse(const FString& InText, FParsed& OutParsed, FString& OutError);

	/** 屏名匹配：空或 `*` 表示任意；否则与类名或控件名忽略大小写相等。 */
	static bool MatchScreen(const FString& InScreen, const FGameTestWidgetNode& InScreenRoot);

	/** 在若干屏根里解析路径；唯一命中才成功，失败时 OutError 带候选。 */
	static bool Resolve(
		const TArray<TSharedPtr<FGameTestWidgetNode>>& InScreens,
		const FParsed& InParsed,
		TSharedPtr<FGameTestWidgetNode>& OutNode,
		FString& OutError);

	/** 解析 + 一步到位（`Resolve` 的便捷包装；@句柄 由调用方处理）。 */
	static bool ResolveText(
		const TArray<TSharedPtr<FGameTestWidgetNode>>& InScreens,
		const FString& InText,
		TSharedPtr<FGameTestWidgetNode>& OutNode,
		FString& OutError);
};
