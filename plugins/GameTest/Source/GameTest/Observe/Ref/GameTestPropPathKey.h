// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * 录制内部挑条目标识仍用 GameTestItemKey 的写法（`by`：`Data.ItemId`、`@self.X`、`@lua.X`、`@item.X`、`@itemLua.X`、`@text`），
 * 只在内存里用、不进脚本；凡是写进脚本、配置（identRules）或回给面板的都是属性路径 v5（docs/game-test-ref.md 3.1）。
 * 两边在这里互转，换算规则只写这一份。
 */
namespace GameTestPropPathKey
{
	/** 一个来源 + 字段路径 → v5 路径：self → `@.X`，lua → `@#X`，item → `@item.X`，itemLua → `@item#X`，text → `@:text`。认不了返回空。 */
	GAMETEST_API FString FromSource(const FString& InSource, const FString& InPath);

	/** 录制的 by → v5 路径。不带 @ 的是默认来源（列表里是条目数据对象 item，其他是 self）。认不了返回空。 */
	GAMETEST_API FString FromBy(const FString& InBy, bool bInList);

	/** v5 路径 → 录制的 by（默认来源不带前缀）。只认条目上的字段和 `@:text`；认不了返回 false。 */
	GAMETEST_API bool ToBy(const FString& InProp, bool bInList, FString& OutBy);
}
