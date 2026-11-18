// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 动画资产的**纳管**：资产先由 DCC 导入（FBX 烘焙），JSON 管元数据与设置的**微调** + 回环。
 *
 * 与 `anim-blueprint`（状态机，图重建）是两件事：那个管"怎么放"，这个管"放的是什么"。
 *
 * 纳管型的三条约定（与骨骼资产三件套一致）：
 * - **资产必须已存在**（找不到报错，不做"凭空生成"）；
 * - 导出**全量**（`ExportStructDelta` 传 `InDefaultData = nullptr`）——配置可以整份手改再导回；
 * - 导入**只写配置里出现的字段**（微调语义）——配置没写的保持资产原值，不重置。
 *
 * 覆盖面（`UAnimSequenceBase` 家族：动画序列与蒙太奇）：`RateScale`、循环、根运动开关、
 * 重定向源、加法动画设置，以及 **Notifies**（`TArray<FAnimNotifyEvent>`，USTRUCT 数组——
 * 增删即改数组，通用编解码直接支持）；蒙太奇的 `Sections` 与槽轨同理。
 *
 * 刻意不碰：采样时长与压缩重采样（那是"生成"不是"微调"）。
 */
namespace AnimSequenceForge
{
	/** 本处理器认领的 kind。 */
	constexpr TCHAR const* Kind = TEXT("anim-sequence");

	ASSETFORGE_API bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport);

	ASSETFORGE_API bool ImportDocument(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport);
}
