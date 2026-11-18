// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 字体（`UFont` + 一组 `UFontFace`）的导入与导出。
 *
 * 一份配置描述一个「复合字体」：根是 `UFont`（Runtime 缓存），`children` 每个是一项
 * `UFontFace`（指向磁盘上的一个 ttf），按配置顺序构成默认字体的 typeface 列表——
 * 同一家族的不同字重/斜体因此合并进**一个** UFont，UI 里通过 typeface 名
 * （`Regular` / `Bold` / `Italic` / ……）选择，这正是引擎自己的复合字体模型
 * （`FCompositeFont::DefaultTypeface.Fonts`），不是自造结构。
 *
 * 配置形状：
 * - `root.class`  = `Font`（短名，按 /Script/Engine 解析）；只支持 Runtime 缓存
 * - `root.props`  = `UFont` 的属性；`FontCacheType` 必须显式写 `"Runtime"`
 * - `root.children[i].class`    = `FontFace`（短名）
 * - `root.children[i].name`     = FontFace 资产名（资产路径 = 与 UFont 同目录 + 该名字）
 * - `root.children[i].source`   = 源 ttf 路径（相对项目根；新面必须写、必须真实存在）
 * - `root.children[i].typeface` = 在 UFont 里的 typeface 名（UI 里选的就是它，全局唯一）
 * - `root.children[i].props`    = `UFontFace` 的属性（Hinting / LoadingPolicy 等）
 *
 * 顺序有意义：第一个 face 是 Slate 的兜底（`FSlateFontInfo` 的 typeface 名为空时用它），
 * 约定放 `Regular`。`source` 与 `typeface` 不是资产属性（分别表达 ttf 与 entry 名），
 * 因此住在节点的扩展字段里（管道不认识它们，只负责保留）。
 */
namespace FontForge
{
	/** 本处理器认领的 kind。 */
	constexpr TCHAR const* Kind = TEXT("font");

	ASSETFORGE_API bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport);

	/** 会就地归一化文档（删掉显式写出的默认值）。 */
	ASSETFORGE_API bool ImportDocument(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport);
}
