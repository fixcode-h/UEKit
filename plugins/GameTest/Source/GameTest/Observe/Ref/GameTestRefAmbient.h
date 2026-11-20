// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FGameTestFlowContext;

/**
 * 「此刻在跑的脚本上下文」：v4 的部件写法（target、toTarget、each、skipIf 的 target）要用场景的 `objects` 和变量解析，
 * 可是控件解析的老入口（TryResolveTarget 等十几处）只拿得到句柄表。与其改每个调用点的签名，执行器在 Tick 里
 * 设一次（作用域对象，游戏线程上用），解析时从这里取；没有脚本上下文时（gt.resolve 单独解析）用临时的空上下文。
 */
namespace GameTestRefAmbient
{
	/** 当前的脚本上下文；没有时为空。 */
	GAMETEST_API FGameTestFlowContext* Get();

	/** 作用域：构造时设、析构时还原（可嵌套）。 */
	struct GAMETEST_API FScope
	{
		explicit FScope(FGameTestFlowContext* InContext);
		~FScope();

	private:
		FGameTestFlowContext* Prev = nullptr;
	};
}
