// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 行为树的导入与导出。
 *
 * 行为树也是树，所以同样复用 `FForgeNode` 与「按名匹配」的增量策略——节点的名字是
 * `UBTNode::NodeName`。
 *
 * 配置形状：
 * - `blackboard` = 黑板资产路径（文档级字段，进 `Extra`；行为树与黑板是多对一）
 * - `root`       = 根节点（`UBehaviorTree::RootNode`），必须是 composite
 * - 每个节点可以有 `decorators` / `services` 两个数组（进 `FForgeNode::Extra`）
 *
 * 装饰器在引擎里挂在「边」上（`FBTCompositeChild::Decorators`），服务挂在下游节点上
 * （`UBTCompositeNode::Services`）——这个区别在配置里被抹平：两边都写在所属节点的
 * `decorators` / `services` 数组里，导入时各归各位。
 */
namespace BehaviorTreeForge
{
	/** 本处理器认领的 kind。 */
	constexpr TCHAR const* Kind = TEXT("behavior-tree");

	/** 行为树的资产类：没有派生，固定这一个。 */
	constexpr TCHAR const* BehaviorTreeClassPath = TEXT("/Script/AIModule.BehaviorTree");

	ASSETFORGE_API bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport);

	/** 会就地归一化文档（删掉显式写出的默认值）。 */
	ASSETFORGE_API bool ImportDocument(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport);
}
