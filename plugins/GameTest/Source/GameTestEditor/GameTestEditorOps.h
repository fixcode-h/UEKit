// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * 编辑器才有的 gt.* 操作：`gt.widgets`（读 WBP 资产控件树）、`gt.api.scan`（重扫 C++ 项目接口）。
 * 操作表见 docs/game-test-script.md 第 10 节。
 */
namespace GameTestEditorOps
{
	void Register();
	void Unregister();
}
