// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** gt.run / gt.debug 给的调试选项（协议第 10 节）。 */
struct GAMETEST_API FGameTestDebugOptions
{
	/** 断点（步骤 id 或路径）：执行到它之前暂停。 */
	TSet<FString> Breakpoints;
	/** 从这一步开始：主流程里它之前的步骤不跑（外层块只走包含它的那一串一次）。 */
	FString From;
	/** 跑到这一步之前暂停。 */
	FString To;
	/** 失败时先暂停等人看，继续后再按 onFail 处理。 */
	bool bPauseOnFail = false;
};
