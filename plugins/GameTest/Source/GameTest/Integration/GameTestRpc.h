// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * GameTest 挂到 UELink `/rpc` 上的操作（`gt.*`）和推送事件（`gt.job`、`gt.record`）。
 * Agent 面板的 GameTest 页和 `ue_gametest` 工具都调它们；操作表见仓库的 `docs/game-test.md`。
 *
 * 文件名不叫 Ops：`Operate/GameTestOps` 已经是「控件操作」的意思。
 */
namespace GameTestRpc
{
	void Register();
	void Unregister();
}
