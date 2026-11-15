// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 骨骼网格体的**纳管**：网格先由 FBX 导入（`Tools/import-qtank.py`）产出，
 * JSON 管参数的**微调** + 回环（导出全量、导入只写出现字段），两者并存、写同一资产。
 *
 * 与其它纳管型 kind 同约定：资产必须已存在；导出**全量**；导入**只写配置里出现的字段**；无动作不写盘。
 *
 * 覆盖面（`USkeletalMesh` 的可编辑标量与结构字段）：网格设置与引用类字段的微调项；
 * `SourceModels` 刻意排除（导入派生数据）。`Sockets` 同 Skeleton：**没有 `Edit` 标志**，天然不进配置。
 */
namespace SkeletalMeshForge
{
	/** 本处理器认领的 kind。 */
	constexpr TCHAR const* Kind = TEXT("skeletal-mesh");

	ASSETFORGE_API bool ExportAsset(
		const FString& InAssetPath,
		FForgeDocument& OutDocument,
		FForgeReport& OutReport);

	ASSETFORGE_API bool ImportDocument(
		FForgeDocument& InDocument,
		bool bDryRun,
		FForgeReport& OutReport);
}
