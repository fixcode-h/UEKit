// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * 每种 kind 的配置 JSON Schema（`forge.kinds kind=<kind>` 返回，Agent 写配置前先取）。
 *
 * 为什么要有：Agent 不知道 `root.class` 该写什么、哪些 kind 不许有 `children`、`skeleton` /
 * `blackboard` 这类文档级字段放哪，只能写一版交给演练、被拒、再改——每份配置白跑一轮。
 * Schema 把这些结构约束一次说清楚；`props` 里的属性名和类型按类反射另取（`ForgePropertySchema`），
 * 不塞进这里：一个类几百个属性，放进每份 schema 会超过返回上限。
 *
 * 结构形状和登记表分开放：登记表管「有哪些 kind、怎么导入导出」，这里只管「配置长什么样」，
 * 没写形状的 kind 给通用 schema（`AssetForge.Schema` 用例会拦住漏写的）。
 */
namespace ForgeKindSchema
{
	/** 这个 kind 有没有专门写的形状（没有时 Build 给通用 schema）。 */
	ASSETFORGE_API bool HasShape(const FString& InKind);

	/**
	 * 一种 kind 的配置文档 schema（JSON Schema 2020-12）。
	 * 根节点 `props` 能按类展开时，在 `x-forge.propsClass` 里写那个类，客户端拿它再调 `forge.kinds class=`。
	 * 未登记的 kind 返回空。
	 */
	ASSETFORGE_API TSharedPtr<FJsonObject> Build(const FString& InKind);
}
