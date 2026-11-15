// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 骨骼资产的**纳管**：骨骼先由 FBX 导入（`Tools/import-qtank.py`）产出，
 * JSON 管参数的**微调** + 回环（导出全量、导入只写出现字段），两者并存、写同一资产。
 *
 * 与其它纳管型 kind 同约定：资产必须已存在；导出**全量**；导入**只写配置里出现的字段**；无动作不写盘。
 *
 * 覆盖面（`USkeleton` 的可编辑标量与结构字段）：`BoneTree`（重定向模式）等微调项。
 * `Sockets` 是对象数组且**没有 `Edit` 标志**（引擎不在细节面板暴露），天然不进配置；
 * 插槽增删由编辑器完成，配置侧的专用读写是后续阶段。
 */
namespace SkeletonForge
{
	/** 本处理器认领的 kind。 */
	constexpr TCHAR const* Kind = TEXT("skeleton");

	ASSETFORGE_API bool ExportAsset(
		const FString& InAssetPath,
		FForgeDocument& OutDocument,
		FForgeReport& OutReport);

	ASSETFORGE_API bool ImportDocument(
		FForgeDocument& InDocument,
		bool bDryRun,
		FForgeReport& OutReport);
}
