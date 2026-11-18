// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 数据资产（`UDataAsset` 派生）的导入与导出。
 *
 * 这一类**没有自己的结构**——一份数据资产就是「一个对象 + 一组属性」，所以结构读写是空的，
 * 全部落在属性编解码上：`ForgePropertyCodec` 原样复用。接入成本因此是最低的一类，
 * 一个处理器覆盖**所有**数据资产（氛围预设、各种调参资产），与具体是什么类无关。
 *
 * 配置形状（复用 UMG 已经用的文档结构，零改动）：
 * - `root.class` = 资产的类，同时用于创建新资产
 * - `root.name`  = 资产名（仅作对照）
 * - `root.props` = 资产的全部可配置属性
 */
namespace DataAssetForge
{
	/** 本处理器认领的 kind。 */
	constexpr TCHAR const* Kind = TEXT("data-asset");

	ASSETFORGE_API bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport);

	/** 会就地归一化文档（删掉显式写出的默认值）。 */
	ASSETFORGE_API bool ImportDocument(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport);
}
