// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * editor.* 动作（1.11.0，录 UE 编辑器操作的回放）：打开、选中、改属性、放 Actor、命令、保存、编译、界面点击、检查。
 *
 * 只在编辑器里有（GameTestEditor 模块），在模块启动时登记到动作表、关闭时摘掉。能复用 UELink 编辑器操作的
 * （asset.open、editor.select、editor.spawn、asset.save、bp.compile）直接经 UELink 调，不另写一份；改属性自己包一个事务，
 * 回放之后在编辑器里 Ctrl+Z 能撤销。动作表见 docs/game-test-script.md 第 15 节。
 */
namespace GameTestEditorActions
{
	void Register();
	void Unregister();
}
