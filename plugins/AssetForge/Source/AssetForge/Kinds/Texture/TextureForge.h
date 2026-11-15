// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 纹理贴图（`UTexture2D`）的导入与导出。
 *
 * 纹理是「源图 + 一整套导入设置」：设置（压缩、sRGB、LODGroup、Mip 生成、过滤与寻址……）
 * 走反射驱动的那套编解码，源图则以 `source` 字段单独表达——它是资产外部的输入，
 * 不是 `UTexture` 上的属性，所以住在节点的扩展字段里（管道不认识它，只负责保留）。
 *
 * 配置形状（复用既有文档结构）：
 * - `root.class`  = `Texture2D`（短名，按 /Script/Engine 解析）或完整路径；只支持 2D 纹理
 * - `root.name`   = 资产名（与 `asset` 的尾名一致；仅作对照）
 * - `root.source` = 源图路径：相对项目根的路径（如 `SourceArt/Atmosphere/T_Xxx.tga`），
 *                   也接受绝对路径。**新纹理必须写**；已有纹理省略则只对齐设置、不动像素
 * - `root.props`  = 纹理的全部可配置属性（反射驱动，与数据资产同一套编解码）
 * - `root.children` 必须为空：纹理没有子节点
 *
 * 源图是内容：导入按「路径是否变化 + 文件是否比上次导入更新」决定要不要重灌像素，
 * 导出把资产导入记录（`UAssetImportData`）里的路径写回 `source`——项目内的转成相对路径。
 */
namespace TextureForge
{
	/** 本处理器认领的 kind。 */
	constexpr TCHAR const* Kind = TEXT("texture");

	ASSETFORGE_API bool ExportAsset(
		const FString& InAssetPath,
		FForgeDocument& OutDocument,
		FForgeReport& OutReport);

	/** 会就地归一化文档（删掉显式写出的默认值）。 */
	ASSETFORGE_API bool ImportDocument(
		FForgeDocument& InDocument,
		bool bDryRun,
		FForgeReport& OutReport);
}
