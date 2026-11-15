// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * Actor 蓝图的导入与导出——只覆盖**组件树**（SimpleConstructionScript）与父类。
 *
 * 组件树天然是一棵树，所以直接复用 `FForgeNode` 的树模型与「按名匹配」的增量策略：
 * 每个组件的名字就是它在 SCS 里的变量名，缺的建、有的改、多的删。
 *
 * 配置形状：
 * - `parent`   = 蓝图父类（C++ 类或另一个蓝图）
 * - `root`     = 占位根节点，`children` 才是组件树
 * - 每个组件：`class` 是组件类**全路径**、`name` 是变量名、`props` 是组件模板的属性
 *
 * **不覆盖的部分**（刻意留白）：事件图表（节点连线）、蓝图变量与函数、接口实现。
 * 那些是"逻辑"而不是"结构"，用文本表达它们的收益远低于成本（见 design 第 10 节）。
 */
namespace ActorBlueprintForge
{
	/** 本处理器认领的 kind。 */
	constexpr TCHAR const* Kind = TEXT("actor-blueprint");

	/** 配置根节点的 class 占位值：它代表「组件树」而不是某个组件。 */
	constexpr TCHAR const* ComponentRootClass = TEXT("ActorComponents");

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
