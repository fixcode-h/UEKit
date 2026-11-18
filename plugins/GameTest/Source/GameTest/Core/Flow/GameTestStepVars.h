// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FGameTestFlowContext;
class FJsonObject;
struct FGameTestNode;

/**
 * 报告里每步的 `payload.vars`（1.11.2）：这一步用到、写到的变量在这一步结束时的值。
 *
 * 只给这一步自己的变量，不给全量快照：报告逐步看「这一步拿到的 $count 是几」，全量快照每步一份太长，
 * 而且看不出哪个和这一步有关。全量快照仍只在调试暂停时给（`gt.debug` 的 `vars`）。
 */
namespace GameTestStepVars
{
	/** 节点里引用的 `$名字`，加上它写的变量（`set` 的 var、`saveAs`、循环的 as）；块不收子步骤里的。 */
	GAMETEST_API void CollectNames(const FGameTestNode& InNode, TSet<FString>& OutNames);

	/** 按名字取当前值；取不到的（已出作用域的循环变量）不写。一个都没有返回空指针。 */
	GAMETEST_API TSharedPtr<FJsonObject> Pick(const FGameTestFlowContext& InContext, const TSet<FString>& InNames, int32 InMaxChars = 2000);
}
