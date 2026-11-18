// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * DataTable 的导入与导出。
 *
 * 与数据资产的区别在于它是**一组同构的行**，所以配置用 `root.children` 表达行：
 * 每个子节点的 `name` 是 RowName、`props` 是那一行的数据。行级增删改因此直接复用
 * `ForgeDiff` 的「按名匹配」——与 UMG 按控件名匹配是同一套逻辑。
 *
 * 配置形状：
 * - `root.class` 固定写 `/Script/Engine.DataTable`
 * - `root.props.rowStruct` = 行结构体路径（如 `/Script/MyGame.FDamageRow`）
 * - `root.children[]` = 行：`name` 是 RowName，`props` 是该行数据
 *
 * 行不写 `class`——它一定是 `rowStruct` 描述的那个结构体，没有第二种可能。
 */
namespace DataTableForge
{
	/** 本处理器认领的 kind。 */
	constexpr TCHAR const* Kind = TEXT("data-table");

	/** `root.class` 的固定取值：DataTable 只有这一个类，写出来是为了让配置自解释。 */
	constexpr TCHAR const* DataTableClassPath = TEXT("/Script/Engine.DataTable");

	/** 行的 `class` 占位值，导入时不解析。 */
	constexpr TCHAR const* RowClassPlaceholder = TEXT("Row");

	ASSETFORGE_API bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport);

	/** 会就地归一化文档（删掉显式写出的默认值，并摘出 rowStruct）。 */
	ASSETFORGE_API bool ImportDocument(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport);
}
