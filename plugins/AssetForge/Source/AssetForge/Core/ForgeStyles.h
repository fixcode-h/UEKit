// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 算法层：命名样式（顶层 `styles` + 节点 `style`）的展开与还原。
 *
 * 配置写法：
 *   "styles": { "NavButton": { "props": { ... }, "slot": { ... } } }
 *   节点上 "style": "NavButton" 或 ["Base", "NavButton"]（后面的覆盖前面的），节点自己写的 props / slot 再覆盖样式。
 *
 * 为什么放在文档解析阶段、对所有 kind 生效：样式只是「把一组 props / slot 抄到多个节点上」的写法，
 * 展开后的文档和手抄一样，后面的导入、演练、差异都不用认它；props / slot 是所有 kind 共有的节点字段，
 * 没必要只给 UMG 开。
 *
 * 合并规则：对象按字段递归合并（`Font: { Size: 20 }` 只改字号，样式里的其他字体字段保留），
 * 数组和标量整个覆盖；字段名按 `ForgeJsonText::NormalizeKey` 认（`bIsVariable` 与 `IsVariable` 是同一个）。
 */
namespace ForgeStyles
{
	/** 节点的 `style` 字段 → 样式名列表。只认非空字符串或非空字符串数组，别的报错（InPath 指到 `style` 字段）。 */
	ASSETFORGE_API bool ParseStyleRefs(const TSharedPtr<FJsonValue>& InValue, const FString& InPath, TArray<FString>& OutNames, TArray<FForgeDiagnostic>& OutDiagnostics);

	/** 校验顶层 `styles`：每个样式是对象，只许写 `props`、`slot`（都是对象）和 `//` 注释。 */
	ASSETFORGE_API bool ValidateStyles(const TSharedPtr<FJsonObject>& InStyles, TArray<FForgeDiagnostic>& OutDiagnostics);

	/**
	 * 把整棵树里节点引用的样式展开进 Props / Slot（递归）。InStyles 可空（没写 styles）。
	 * 引用了不存在的样式名报错，路径指到那个节点的 `style`。
	 */
	ASSETFORGE_API bool ExpandTree(FForgeNode& InOutNode, const FString& InNodePath, const TSharedPtr<FJsonObject>& InStyles, TArray<FForgeDiagnostic>& OutDiagnostics);

	/**
	 * 写回方向：复制一棵树，引用了样式的节点去掉和样式相同的属性（对象逐字段比，比较按规范形态）。
	 * 不改入参——Props 是共享指针，文档别处还在用。
	 */
	ASSETFORGE_API FForgeNode StripTree(const FForgeNode& InNode, const TSharedPtr<FJsonObject>& InStyles);

	/**
	 * 导出时保留原配置的样式：把 InExisting 的 `styles` 和各节点的 `style` 引用搬到刚从资产导出的文档上
	 * （根对根，子节点按 name 逐层对上；对不上的节点不带样式）。写回时 `StripTree` 再去掉重复的属性。
	 * 导出不反推样式：只保留手写过的。
	 */
	ASSETFORGE_API void CarryStyles(const FForgeDocument& InExisting, FForgeDocument& InOutExported);

	/** 递归合并两个对象，返回新对象（不改入参）：同名对象字段递归，其余 InOver 覆盖 InBase。两边都可空。 */
	ASSETFORGE_API TSharedPtr<FJsonObject> MergeObjects(const TSharedPtr<FJsonObject>& InBase, const TSharedPtr<FJsonObject>& InOver);
}
