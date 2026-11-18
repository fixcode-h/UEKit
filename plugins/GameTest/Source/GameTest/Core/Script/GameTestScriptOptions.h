// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** 脚本的运行开关（`_settings.json` 第 9 节）：由子系统读运行配置后填进上下文，扩展点只读。 */
struct GAMETEST_API FGameTestScriptOptions
{
	/** 步骤（invoke、Lua.Call）能调 Lua 函数。 */
	bool bAllowLuaInvoke = true;
	/** 条件、表达式能调 Lua 函数：Lua 函数有没有副作用看不出来，默认不许。 */
	bool bAllowLuaInCond = false;
	/** `call` 最多嵌套几层。 */
	int32 MaxCallDepth = 8;
	/** 条件默认的稳住时间（毫秒），见协议第 4.2 节。 */
	int32 SettleMs = 300;
	int32 ElseSettleMs = 1000;
};
