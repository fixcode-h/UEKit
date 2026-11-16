// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * 插件自带示例的安装：把 `Plugins/AssetForge/docs/AssetForge/Test` 装进项目。
 *
 * 示例跟着插件走（面板安装 / 升级插件时一起换掉），但配置要在**配置根**里 AssetForge 才扫得到，
 * GameTest 场景要在**场景根**里 GameTest 才列得出，所以启动时拷过去：
 * - `Test/` 下的全部文件 → `<配置根>/Test/`（`_GameTest/` 以 `_` 开头，配置扫描会跳过）
 * - `Test/_GameTest/` 下的场景 → `<GameTest 场景根>/AssetForgeSample/`（项目开了 GameTest 才拷）
 *
 * 只补缺的、只更新「上次装进去以后没人改过」的：装过的每个文件的哈希记在 `Saved/AssetForge/samples.json`，
 * 用户改过的不覆盖，删掉的不再装回来（想重装就删掉这份记录）。
 */
namespace ForgeSamples
{
	/** 插件目录里示例的位置（磁盘绝对路径）；找不到插件时为空。 */
	ASSETFORGE_API FString GetSourceDir();

	/** 按上面的规则安装；项目设置「安装示例测试界面」关掉时什么都不做。返回这次写了几个文件。 */
	ASSETFORGE_API int32 Install();
}
