// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 非 Actor 蓝图类（UObject 蓝图）的导入与导出——覆盖父类与 CDO 上的可编辑属性。
 *
 * 它是 `actor-blueprint` 在"非 Actor"一侧的对偶：Actor 蓝图管组件树（SimpleConstructionScript），
 * 这里管**没有组件树的蓝图类**——GameplayAbility、GameplayEffect 以及将来任何"父类不是 AActor"
 * 的蓝图类（首用场景见 openspec/changes/asset-forge §19）。
 *
 * 配置形状：
 * - `parent` = 蓝图父类（C++ 类或另一个蓝图）；必须「非 AActor 派生、可蓝图化、非抽象」
 * - `root`   = 资产自身：`props` 直接是 CDO 属性；`children` 必须为空（这里没有组件树）
 *
 * **不覆盖的部分**（刻意留白）：事件图表（节点连线）、蓝图变量与函数、接口实现——
 * 与 actor-blueprint 的理由相同：那是"逻辑"而不是"结构"，文本化的收益远低于成本。
 */
namespace ObjectBlueprintForge
{
	/** 本处理器认领的 kind。 */
	constexpr TCHAR const* Kind = TEXT("object-blueprint");

	/** 配置根节点的 class 占位值：它代表「类的默认值」而不是某个对象。 */
	constexpr TCHAR const* ObjectRootClass = TEXT("ClassDefaults");

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
