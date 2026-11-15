// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 编排层：UMG 资产的导入与导出。
 *
 * 两条方向的对称性是有意的：导出与导入共用同一套编解码（UmgWidgetCodec + ForgePropertyCodec），
 * 所以「导出→导入」应当是无损往返。「纳管已有资产」与「从配置生成」因此不会出现两套语义。
 *
 * 导入是**增量**的：按控件名匹配，缺的建、有的改、多的删、顺序变的重排。
 * 清空重建虽然更简单，但会让动画、绑定与手工调过的 Slate 属性全部失效——
 * 而且增量才能让 dry-run 给出有意义的计划。
 */
namespace UmgForge
{
	/** 本处理器认领的 kind。 */
	// constexpr 在 C++17 下隐式 inline：头文件里的常量这样写才不会每个 TU 各来一份定义。
	constexpr TCHAR const* Kind = TEXT("umg-widget");

	/**
	 * 导出：`/Game/...` 的 UMG 资产 → 配置文档。
	 *
	 * 只写与 CDO 不同的属性，所以导出结果干净、可 diff，也不会在引擎改默认值时整片抖动。
	 */
	ASSETFORGE_API bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport);

	/**
	 * 导入：配置文档 → UMG 资产。
	 *
	 * bDryRun 为真时只计算计划并返回，不创建资产、不修改包。AI 应当先跑 dry-run 确认计划，
	 * 再真跑——这是它敢批量改配置的前提。
	 */
	/** 注意：会就地归一化文档（把显式写出的默认值删掉），见 ForgePropertyCodec::PruneDefaults。 */
	ASSETFORGE_API bool ImportDocument(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport);

	/** 读出资产当前的控件树（供漂移检测与外部工具使用）。 */
	ASSETFORGE_API bool ReadAssetTree(const FString& InAssetPath, FForgeNode& OutRoot, FForgeReport& OutReport);
}
