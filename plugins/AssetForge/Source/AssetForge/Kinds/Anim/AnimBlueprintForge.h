// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 动画蓝图的导入与导出——只覆盖 AnimGraph 里的**状态机**。
 *
 * 状态机是**图**不是树：状态之间的转移由引脚连接表达，没有独立的转移数据
 * （`UAnimStateNodeBase::GetTransitionList` 就是遍历引脚算出来的）。所以状态列表走 `children`
 * 的树模型，转移用 `extra.transitions` 单独列出，入口状态用 `extra.entry`。
 *
 * 配置形状：
 * - `skeleton`（顶层）= 目标骨架；没有它动画蓝图编译不过
 * - `parent` = 动画实例父类，缺省 `/Script/Engine.AnimInstance`
 * - `root` = 占位根（`class` = `AnimGraph`），`children` 是状态机
 * - 每个状态机：`class` = `StateMachine`、`name` = 状态机图名、`children` = 状态、`extra` 见下
 * - 每个状态：`class` = `State`、`name` = 状态名、`props.sequence` = 动画序列、`props.loop`、`props.playRate`
 * - `extra.entry` = 入口状态名（缺省取第一个状态）、`extra.transitions` = `{ from, to, crossfade, autoRule }`
 *
 * **不覆盖的部分**（刻意留白）：AnimGraph 里状态机之外的节点（混合、IK、同步组…）、
 * 转移规则图里的节点连线、状态别名（conduit）与通道。理由同 actor-blueprint：
 * 那是逻辑而不是结构，用文本表达的收益远低于成本。
 */
namespace AnimBlueprintForge
{
	/** 本处理器认领的 kind。 */
	constexpr TCHAR const* Kind = TEXT("anim-blueprint");

	/** 配置根节点的 class 占位值：它代表「AnimGraph」而不是某个状态机。 */
	constexpr TCHAR const* AnimGraphRootClass = TEXT("AnimGraph");

	/** 状态机节点的 class 名。 */
	constexpr TCHAR const* StateMachineClass = TEXT("StateMachine");

	/** 状态节点的 class 名。 */
	constexpr TCHAR const* StateClass = TEXT("State");

	ASSETFORGE_API bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport);

	/** 会就地归一化文档（摘掉骨架等扩展字段的默认写法）。 */
	ASSETFORGE_API bool ImportDocument(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport);
}
