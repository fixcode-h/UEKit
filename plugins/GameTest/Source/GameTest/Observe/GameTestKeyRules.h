// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** 一条「条目怎么认」规则（录制挑条目标识用；字段写法是录制内部的 by：默认来源不带 @）。 */
struct GAMETEST_API FGameTestKeyRule
{
	FString Entry;
	FString Container;
	TArray<FString> By;
};

/**
 * 运行设置里的「这类对象按什么认」规则：2.0.0 起存在 `_settings.json` 的 identRules（docs/game-test-ref.md 第 6 节），
 * 这里把它换成录制内部的 by 写法（`@item.X` ↔ `X`）。
 *
 * 录制时每点一次都要查，所以缓存一份，文件改了（面板运行设置保存、录制时勾「以后都按这个认」）才重读。
 * 只在游戏线程上用。
 */
namespace GameTestKeyRules
{
	/** 这类条目在这个容器里有没有规则：先找写了容器、和 Base 对得上的，再找没写容器的。没有返回 false。 */
	GAMETEST_API bool Find(const FString& InEntryClass, const FString& InBase, FGameTestKeyRule& OutRule);

	/** 加一条（同一个条目类 + 容器的旧规则换掉），写回 `_settings.json`。 */
	GAMETEST_API bool Save(const FGameTestKeyRule& InRule, FString& OutError);

	/** 类名比较：`WBP_BagSlot` 和 `WBP_BagSlot_C` 算同一个，忽略大小写。 */
	GAMETEST_API bool SameClass(const FString& InLeft, const FString& InRight);
}
