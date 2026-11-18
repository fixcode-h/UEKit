// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/GameTestTypes.h"

class FGameTestFlowContext;

/**
 * 叶子步骤的填值求值（协议第 1.1 节）：开步前把字段里的 `$var`、`=表达式`、`{"$obj"}` 换成值，
 * 选择器字段（target、toTarget…）里的控件变量换成临时句柄。动作照 1.x 的写法读字段，不用知道有变量这回事。
 *
 * 不碰的字段：条件和表达式（cond、expr、expect、until、skipIf、guards——它们每帧自己求）、
 * 自己按类型处理参数的（api / invoke 的 args、object、with），以及公共字段。
 */
namespace GameTestLeafRefs
{
	/** `InOutStep.Params` 换成求过值的副本（没有引用时不复制）；Target 同步。取不到返回 false 和原因。 */
	GAMETEST_API bool Resolve(FGameTestStep& InOutStep, FGameTestFlowContext& InContext, FString& OutError);
}
