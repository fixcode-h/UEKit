// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * 算法层：源文件路径在「配置写法」与「磁盘绝对路径」之间的换算。
 *
 * 配置里写**相对项目根**的路径（`SourceArt/...`），资产属性里存**绝对路径**——两个方向
 * 都要能把对方的形式解释对。这份实现被多个 kind 共用（texture、font），因为它们对源文件
 * 的处理完全同构：源文件是资产外部的输入，读取时按需重灌，导出时写回配置相对形式。
 */
namespace ForgePaths
{
	/**
	 * 统一成「绝对 + 正斜杠 + 折叠过 ..」的形态，方便比较与写配置。
	 *
	 * `FPaths::CollapseRelativeDirectories` 不消盘符后紧跟的 `..`（实测 `E:/../x` 原样返回），
	 * 这里手动再收一遍——老资产的导入记录里什么形态都可能出现。
	 */
	ASSETFORGE_API FString CollapseToAbsolute(const FString& InPath);

	/**
	 * 绝对路径 → 配置里的写法：项目内的转相对（正斜杠），项目外原样。
	 *
	 * 不用 `FPaths::MakePathRelativeTo`：它的基准语义与"相对项目根"对不上（实测会把
	 * `SourceArt/x.tga` 写成 `../../../SourceArt/x.tga`），这里按前缀手动裁。
	 */
	ASSETFORGE_API FString MakeProjectRelative(const FString& InAbsolutePath);
}
