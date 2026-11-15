// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 算法层：配置文档与其 JSON 表示之间的双向转换。
 *
 * 结构字段用小写短名（`kind` / `asset` / `class` / `name` / `props` / `slot` / `children`），
 * 属性字段用 UE 属性名原样（`Visibility` / `RenderOpacity` / `Anchors`）——后者零映射成本：
 * 反射直接查得到，AI 查引擎文档也能对上，导出再导入不漂移。
 */
namespace ForgeDoc
{
	/** 当前支持的 schema 版本。改格式时 +1，老版本走显式迁移而不是猜测。 */
	constexpr int32 CurrentSchemaVersion = 1;

	/** 解析一份配置文档；诊断带字段级路径。 */
	ASSETFORGE_API bool Parse(
		const TSharedPtr<FJsonObject>& InJson,
		const FString& InSourcePath,
		FForgeDocument& OutDocument,
		TArray<FForgeDiagnostic>& OutDiagnostics);

	/** 把文档写回 JSON（保留 Extra 里的未识别字段）。 */
	ASSETFORGE_API TSharedPtr<FJsonObject> ToJson(const FForgeDocument& InDocument);

	/** 单节点解析/序列化，供导出方向与测试直接使用。 */
	ASSETFORGE_API bool ParseNode(
		const TSharedPtr<FJsonObject>& InJson,
		const FString& InNodePath,
		FForgeNode& OutNode,
		TArray<FForgeDiagnostic>& OutDiagnostics);

	ASSETFORGE_API TSharedPtr<FJsonObject> NodeToJson(const FForgeNode& InNode);
}
