// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * Niagara 系统的导入与导出——**模板 Emitter 复制 + 系统层用户参数覆盖**，不做模块图操作。
 *
 * 配置形状：
 * - `root`        = 系统自身。`props` 是**系统层用户参数**的默认值（键 = 参数全名，如 `User.SpriteSize`）；
 *                   `children` 是发射器清单。
 * - `children[i]` = 一个发射器：`class` 是**模板 Emitter 的资产路径**（复制源）、`name` 是发射器名
 *                   （导入时的匹配键）、`props` 必须为空。
 *
 * 刻意留白：模块图（加模块、连引脚）、渲染器内部、事件与模拟阶段、系统自有属性——
 * 前者是"图"而不是"结构"，后者等有真实消费者时再接。发射器的形状只能来自模板：
 * 本类型决定的是**用哪些模板、叫什么名字、参数覆盖成什么值**。
 *
 * 已知取舍（首版）：发射器按**名**匹配，同名不重建——换模板请换名字或先删掉再导入；
 * 导出方向的"模板来源"取自配置文件（按名对照），配置不在时会留空并给 warning。
 */
namespace NiagaraForge
{
	/** 本处理器认领的 kind。 */
	constexpr TCHAR const* Kind = TEXT("niagara");

	/** 配置根节点的 class 占位值。 */
	constexpr TCHAR const* SystemRootClass = TEXT("NiagaraSystem");

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
