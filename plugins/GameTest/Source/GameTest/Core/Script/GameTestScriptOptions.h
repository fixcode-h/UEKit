// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonValue;

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

	//~ 对象引用（docs/game-test-ref.md 第 5 节）-------------------------------
	/** 引用找到多个时默认怎么办：error / first / last / nearest。 */
	FString RefMany = TEXT("error");
	/** 文字比较默认忽略大小写。 */
	bool bRefIgnoreCase = true;
	/** eq / near 的默认容差：float、vector（厘米）、rotator（度）、color（0–1 分量）。 */
	double TolFloat = 0.001;
	double TolVector = 10.0;
	double TolRotator = 1.0;
	double TolColor = 0.02;
	/** obj.close 关完确认没了的默认等待（毫秒）。 */
	int32 CloseVerifyMs = 2000;
	/** obj.close 许不许用 destroy（直接销毁 Actor）。 */
	bool bAllowDestroy = false;
	/** `identRules`、`closeRules` 原样（JSON 数组），录制和 obj.close 用。 */
	TSharedPtr<FJsonValue> IdentRules;
	TSharedPtr<FJsonValue> CloseRules;
};
