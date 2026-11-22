// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FGameTestScriptParser;
class FJsonObject;
struct FGameTestNode;

/**
 * 附带 GM（步骤的 `hooks` 字段，2.3.0）：`{before?, after?, fail?}`，每组是步骤列表（一般是 `gm.run`）。
 *
 * 解析时把带 hooks 的步骤包一层（`bHookWrap` 的节点，路径 `<步骤路径>#hooks`）：主步骤原样放在里面，路径、id 不变，
 * 断点、报告照旧按它认。执行器不认识 hooks，只多了一个通用的「块接住子节点失败」（IGameTestBlock::CatchFailure）。
 */
namespace GameTestHookWrap
{
	/** `InJson` 带 hooks 时解析成包了一层的节点；`disabled` 的步骤不包（整步不跑，附带的也不跑）。 */
	TSharedRef<FGameTestNode> Wrap(FGameTestScriptParser& InParser, const TSharedPtr<FJsonObject>& InJson, const FString& InPath);

	/** 包了一层时返回里面的主步骤，否则返回它自己（ParseNodes 给叶子填 bNested / Index 用）。 */
	FGameTestNode& Main(FGameTestNode& InNode);
}
