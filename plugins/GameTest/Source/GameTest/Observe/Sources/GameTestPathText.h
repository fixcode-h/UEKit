// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Observe/Sources/GameTestValueSource.h"

class FGameTestFlowContext;

/**
 * 纯路径文字 → 段：`Data.Items[0]["key"].GetCount()`。
 *
 * 给不经过表达式解析器的地方用：定位器 key（`Data.ItemId`）、`@lua.x` 的路径、`gt.vars` 的 `path`。
 * 只认成员、数字 / 字符串下标、无参调用；带参数的调用、运算写在表达式里（`=…`）。
 */
namespace GameTestPathText
{
	/** 拆不开（括号不配对、空段）时返回 false 并写原因。开头的 `.` 可有可无。 */
	GAMETEST_API bool Parse(const FString& InText, TArray<FGameTestPathSegment>& OutSegments, FString& OutError);

	/**
	 * `@来源[选择器].路径` 整条文字 → 来源名、选择器原文、段（`@sub.UQuestSubsystem.ActiveQuests`、`@actor[tag=Boss].Health`）。
	 * 不以 `@` 开头返回 false。
	 */
	GAMETEST_API bool ParseSource(const FString& InText, FName& OutSource, FString& OutSelector, TArray<FGameTestPathSegment>& OutSegments, FString& OutError);

	/** 整条 `@来源…` 文字直接取值（解析 + GameTestPath::ResolveSource）。解析不了时 Unresolved（expr_syntax）。 */
	GAMETEST_API FGameTestValue ResolveSourceText(const FString& InText, FGameTestFlowContext& InContext);

	/** 段 → 文字（Parse 的反过程，报告和 gt.vars 的 path 用）。 */
	GAMETEST_API FString Join(TArrayView<const FGameTestPathSegment> InSegments);
}
