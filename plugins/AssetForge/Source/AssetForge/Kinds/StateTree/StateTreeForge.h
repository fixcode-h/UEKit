// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

// 只在 ASSETFORGE_WITH_STATETREE 时有内容（UE 5.8），见 AssetForge.Build.cs
#if ASSETFORGE_WITH_STATETREE

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 状态树（StateTree）的导入与导出。
 *
 * 与行为树同路数：状态做 `FForgeNode` 树（children = 子状态），任务 / 进入条件 /
 * 考量 / 转移等列表走节点自带的 JSON 通道（`Extra`），树差异只到"状态"这一层。
 *
 * 同步策略（与其余 kind 的差异在这里）：
 * - 状态按名字匹配（保留状态 ID，转移与绑定的引用才稳定）；改名 = 删旧建新。
 * - 节点"名字优先、位置兜底"复用；绑定与参数每次导入全量重建（配置真源）。
 * - 节点属性写实例值层（编译读取 `FStateTreeEditorNode::GetInstance()`）。
 *
 * 引用格式沿用引擎自身的拼接：状态路径 `Root/Combat/Attack`、可绑定引用
 * `StatePath/Name`（如 `Global/Parameters`、`Root/Combat/Wait`），属性以 `.` 追加。
 *
 * 配置形状与字段约束见 skill `tk-state-tree-config`。
 */
namespace StateTreeForge
{
	/** 本处理器认领的 kind。 */
	constexpr TCHAR const* Kind = TEXT("state-tree");

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

#endif // ASSETFORGE_WITH_STATETREE
