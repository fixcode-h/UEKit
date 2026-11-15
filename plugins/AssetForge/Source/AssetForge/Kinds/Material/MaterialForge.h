// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 材质（UMaterial）的导入与导出——覆盖材质图本身：材质属性输入 + 表达式节点 + 属性。
 *
 * 配置形状（`nodes` / `inputs` 是 root 节点的扩展字段，见 MaterialGraphCodec）：
 * ```json
 * {
 *   "kind": "material",
 *   "schemaVersion": 1,
 *   "revision": 0,
 *   "asset": "/Game/Materials/M_X",
 *   "root": {
 *     "class": "Material",
 *     "name": "M_X",
 *     "props": { "BlendMode": "BLEND_Masked", "TwoSided": true },
 *     "inputs": { "BaseColor": "Tint", "Roughness": { "from": "Tex", "output": "R" } },
 *     "nodes": [
 *       { "name": "Tint", "class": "VectorParameter",
 *         "props": { "ParameterName": "Color", "DefaultValue": { "R": 1, "G": 0.5, "B": 0.2, "A": 1 } } },
 *       { "name": "Tex", "class": "TextureSample", "inputs": { "UVs": "UV" },
 *         "props": { "Texture": "/Game/Textures/T_X.T_X" } }
 *     ]
 *   }
 * }
 * ```
 *
 * 三件跟别的 kind 不一样的事，都在这里有意的决定：
 *
 * 1. **节点位置（坐标）不进配置**。排版是表现层，写进配置就意味着「在编辑器里拖一下节点」
 *    变成一次配置差异；而收敛判据建立在内容等价上。导入之后由**引擎自己的排版**收尾：
 *    `UMaterialEditingLibrary::LayoutMaterialExpressions` 就是材质编辑器里「重整节点」
 *    那套按深度分列的算法，孤儿节点它不管，我们另外摆。
 * 2. **执行策略是整体重建**：先清空表达式集合，再按配置建一遍（与动画蓝图状态机同理）。
 *    内容等价时**一个动作都不做**，所以常见路径上不会白重建、也不动文件时间戳。
 * 3. **注释节点（Comment）不在契约里**：导出跳过、导入不动它们（注释是给人看的标注，
 *    删掉比留着更刺眼；它也不参与着色）。
 *
 * 不覆盖的部分（刻意留白）：材质实例（UMaterialInstanceConstant，另一个资产类型）、
 * 材质函数（UMaterialFunction，图的出入口不是材质属性而是它自己的输入输出）、
 * 折叠图（Composite，表达不了）、自定义表达式里的 HLSL 文本与引脚（走 props，能写但
 * 引脚由属性决定）。
 */
namespace MaterialForge
{
	/** 本处理器认领的 kind。 */
	constexpr TCHAR const* Kind = TEXT("material");

	/** `root.class` 的短名写法——材质自己就是根节点，没有父类一说。 */
	constexpr TCHAR const* MaterialClass = TEXT("Material");

	ASSETFORGE_API bool ExportAsset(
		const FString& InAssetPath,
		FForgeDocument& OutDocument,
		FForgeReport& OutReport);

	/** 会就地归一化文档（摘掉属性里显式写出的默认值）。 */
	ASSETFORGE_API bool ImportDocument(
		FForgeDocument& InDocument,
		bool bDryRun,
		FForgeReport& OutReport);
}
