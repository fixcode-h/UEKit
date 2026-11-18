// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 地图（`.umap`）的导入与导出——**范围只到世界设置**。
 *
 * 关卡里摆了什么（Actor、地形、关卡蓝图）不归这里管，那些是编辑器里的工作；配置覆盖的是
 * 「这张地图怎么运行」：重力与 KillZ、光照与 Lumen 参数、GameMode 覆盖……
 * 也就是 World Settings 面板上的东西。目标地图不存在时，导入会创建一张**只含世界设置的空地图**。
 *
 * 世界设置的类由地图自己决定（`UWorld::WorldSettingsClass`），配置只是声明自己写的是哪个类；
 * 两者不一致时导入报错，而不是替人换类——换类是编辑器里的操作。
 *
 * 配置形状（复用既有文档结构）：
 * - `root.class` = 世界设置的类（`/Script/Engine.WorldSettings` 或它的派生类）
 * - `root.name`  = 固定 `WorldSettings`（仅作对照）
 * - `root.props` = 世界设置的全部可配置属性（反射驱动，与数据资产同一套编解码）
 * - `root.children` 必须为空：地图内容不由本处理器表达
 */
namespace MapForge
{
	/** 本处理器认领的 kind。 */
	constexpr TCHAR const* Kind = TEXT("map");

	ASSETFORGE_API bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport);

	/** 会就地归一化文档（删掉显式写出的默认值）。 */
	ASSETFORGE_API bool ImportDocument(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport);
}
