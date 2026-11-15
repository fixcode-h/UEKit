// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Registry/ForgeKindRegistry.h"

class UObject;
class FJsonObject;
struct FForgeDocument;

/**
 * 「配置应用后」的对象快照与属性差异：面板 AssetForge 页「属性差异」和 `forge.diff` 用。
 *
 * 做法：把真实资产复制到瞬态包，把配置写进副本，再和真实资产（或另一份配置的副本）逐属性比，
 * 结果是 JSON（属性名、之前、之后），网页上并排显示。
 * 这段逻辑决定了一件**必须被自动验证**的事：**副本不能影响真实资产**，测试模块对真实资产做
 * 「导出 → 造副本 → 再导出」的前后比对守着它。
 */
namespace ForgeDiffSnapshot
{
	/**
	 * 复制一份 `InTarget` 到瞬态包，并把配置写进去。
	 *
	 * **不走各 kind 的 `ImportDocument`**：它按配置里的**资产路径**查对象
	 * （`FindDataAsset(InDocument.Asset)` 那类），副本不在那个路径上——调它只会找到
	 * **真实资产**并在结尾 `SaveAssetPackage` 落盘。这里走的是属性写入那一步：
	 * `ResetStructToDefaults`（按需）+ `ImportStructProperties`。
	 *
	 * 配方（要不要先重置、编解码用对象类还是父类）由 `InSupport` 给出，它与真实导入的
	 * 一致性由 `ForgeEditorUtilsTest` 的用例守着。
	 *
	 * @return 副本；`InTarget` 为空或复制失败时为空。
	 */
	ASSETFORGE_API UObject* MakeConfiguredCopy(
		UObject* InTarget,
		const FForgeDocument& InDocument,
		const ForgeKindRegistry::FForgeDiffSupport& InSupport);

	/**
	 * 对象的包是不是瞬态包。
	 *
	 * 这是「副本不会污染资产」的判据：只有瞬态包里的对象才保证不被存盘。
	 * 不是瞬态就说明复制落到了别的 Outer 上，必须放弃对比。
	 */
	ASSETFORGE_API bool IsTransientObject(const UObject* InObject);

	/** 按 kind 的差异来源找到被比较的对象（资产自身 / 蓝图 CDO / 地图的世界设置）；资产不存在时为空。 */
	ASSETFORGE_API UObject* ResolveTarget(
		const FString& InAssetPath,
		const ForgeKindRegistry::FForgeDiffSupport& InSupport);

	/**
	 * 属性差异（JSON）：
	 * `{"supported", "reason"?, "baseline": "asset"|"config", "changes": [{"property", "before", "after"}], "truncated"?}`。
	 *
	 * `InBaseline` 为空时和真实资产比（「导入会改哪些属性」）；不为空时和那份配置应用后的样子比
	 * （面板从 git 取上次提交的配置传进来：「这次改配置改了哪些属性」）。
	 * 不支持的 kind、资产还不存在时 `supported=false` 并写清原因：空列表会被读成「没有差异」。
	 */
	ASSETFORGE_API TSharedRef<FJsonObject> DiffProperties(
		const FString& InAssetPath,
		const FForgeDocument& InCurrent,
		const FForgeDocument* InBaseline,
		int32 InMaxChanges);
}
