// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 物理资产的**纳管**：形状先由 引擎工具产出，
 * JSON 管参数的**微调** + 回环——命令负责「重建全部」，配置负责「精确增删改」，两者并存、写同一资产。
 *
 * 与其它纳管型 kind 同约定：资产必须已存在；导出**全量**；导入**只写配置里出现的字段**；无动作不写盘。
 *
 * 覆盖面（`UPhysicsAsset` 的标量与结构字段）：资产的开关类参数与约束模板的默认实例参数。
 * `SkeletalBodySetups` / `ConstraintSetup` 是对象数组且**没有 `Edit` 标志**（引擎不在细节面板暴露），
 * 天然不进配置——它们先由命令侧维护，专用读写（增删碰撞体）作为后续阶段。
 */
namespace PhysicsAssetForge
{
	/** 本处理器认领的 kind。 */
	constexpr TCHAR const* Kind = TEXT("physics-asset");

	ASSETFORGE_API bool ExportAsset(
		const FString& InAssetPath,
		FForgeDocument& OutDocument,
		FForgeReport& OutReport);

	ASSETFORGE_API bool ImportDocument(
		FForgeDocument& InDocument,
		bool bDryRun,
		FForgeReport& OutReport);
}
