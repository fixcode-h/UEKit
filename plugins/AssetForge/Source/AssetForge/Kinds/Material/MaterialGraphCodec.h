// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

class UMaterial;
class UMaterialExpression;

/**
 * 材质图 ↔ 配置 JSON。
 *
 * 材质图是 **DAG**：一个表达式可以被多个消费者引用（同一个 TextureSample 同时喂 BaseColor
 * 与 Roughness 是常态），节点之间既没有顺序也没有名字——资产里的表达式只有类、属性与引脚连线。
 *
 * 于是两件事跟别的 kind 不一样：
 *
 * - **节点名由配置自己发明**，它就是匹配键（其它 kind 用的是资产里本来就有身份：控件名、
 *   兄弟顺序、状态名）。引用统一写名字，共享节点就是多处引用同一个名字。
 * - **判等要先把两边名字规范化成结构路径**：从材质属性输入出发做深度优先，
 *   第一段是属性名、后面是输入引脚名（`BaseColor`、`BaseColor.A.Coordinates`）。导出的名字
 *   就是这么来的；手写配置可以另起短名（`Tex1`），规范化后与导出形态逐字段可比，
 *   「名字不同、图一样」不会被误报成改动。
 *
 * 走不到任何材质属性输入的表达式（孤儿节点，艺术家临时停放的）排在节点列表后面，
 * 规范名是 `#0`、`#1`…（按节点列表里的相对次序）。**注释节点（Comment）不在这个契约里**：
 * 导出跳过、导入不动它们。
 */
namespace MaterialGraphCodec
{
	/** 一条连线：源节点名 + 它的输出引脚名（空串 = 第一个输出）。 */
	struct ASSETFORGE_API FMaterialLink
	{
		FString NodeName;
		FString OutputName;
	};

	/** 一个表达式的配置。Inputs 保持书写顺序，诊断与写回都按它来。 */
	struct ASSETFORGE_API FMaterialNodeSpec
	{
		FString Name;
		FString ClassName;
		TSharedPtr<FJsonObject> Props;

		/** 输入引脚名 → 连线。 */
		TArray<TPair<FString, FMaterialLink>> Inputs;

		const FMaterialLink* FindInput(const FString& InPinName) const;
	};

	/** 一张材质图：材质属性输入 + 表达式节点。 */
	struct ASSETFORGE_API FMaterialGraph
	{
		/** 键 = EMaterialProperty 去掉 `MP_` 前缀（`BaseColor`）。 */
		TArray<TPair<FString, FMaterialLink>> PropertyInputs;

		TArray<FMaterialNodeSpec> Nodes;

		const FMaterialNodeSpec* FindNode(const FString& InName) const;
	};

	/** 表达式类名 → UClass。短名按 `/Script/Engine.MaterialExpression<名>` 解析。 */
	ASSETFORGE_API UClass* ResolveExpressionClass(const FString& InClassName, FString& OutError);

	/**
	 * 材质属性名 → EMaterialProperty 的数值。
	 *
	 * 三种写法等价：`BaseColor`、`MP_BaseColor`、`basecolor`。认不出返回 INDEX_NONE。
	 * 返回 int32 而不是枚举本身，是为了不把引擎的 SceneTypes.h 拉进这个头文件。
	 */
	ASSETFORGE_API int32 ResolveMaterialProperty(const FString& InName);

	/** 与 ResolveMaterialProperty 相反的写法：枚举值 → 配置里的键（`MP_` 前缀已去掉）。 */
	ASSETFORGE_API FString MakeMaterialPropertyKey(int32 InProperty);

	/** 输入引脚名：与材质编辑器显示的一致（也就是连接 API 认识的那个名字）。 */
	ASSETFORGE_API FString GetInputPinName(UMaterialExpression* InExpression, int32 InInputIndex);

	/** 输出引脚名：具名输出用它自己的名字，单通道掩码输出给 `R`/`G`/`B`/`A`，其余留空。 */
	ASSETFORGE_API FString GetOutputPinName(const UMaterialExpression* InExpression, int32 InOutputIndex);

	/** 输入引脚名 → 引脚序号（`GetInputPinName` 写的名字都认，另有 `#序号` 写法）。找不到返回 INDEX_NONE。 */
	ASSETFORGE_API int32 ResolveInputIndex(UMaterialExpression* InExpression, const FString& InPinName);

	/** 输出引脚名 → 序号：空串与 `RGB` 都指第一个输出。找不到返回 INDEX_NONE。 */
	ASSETFORGE_API int32 ResolveOutputIndex(UMaterialExpression* InExpression, const FString& InOutputName);

	/** 配置的 `inputs` / `nodes` 两个字段 → 图。 */
	ASSETFORGE_API bool ParseGraph(
		const TSharedPtr<FJsonObject>& InExtra,
		FMaterialGraph& OutGraph,
		TArray<FForgeDiagnostic>& OutDiagnostics,
		const FString& InFieldPath);

	/** 图 → `inputs` / `nodes`（写进 root 的 Extra；空的部分不写）。 */
	ASSETFORGE_API void WriteGraph(const FMaterialGraph& InGraph, TSharedPtr<FJsonObject>& OutExtra);

	/**
	 * 资产 → 图，名字已经是结构路径。
	 *
	 * 材质图里放不进这个契约的节点（Composite 折叠图及其内部的引脚基座）会直接报错：
	 * 它们的信息不只在线连上，导出再导入会静默丢掉折叠结构。
	 */
	ASSETFORGE_API bool ReadGraph(
		UMaterial* InMaterial,
		FMaterialGraph& OutGraph,
		TArray<FForgeDiagnostic>& OutDiagnostics);

	/** 把图里所有名字与引用改写成结构路径（判等的两边都要做）。 */
	ASSETFORGE_API void CanonicalizeNames(FMaterialGraph& InOutGraph);

	/**
	 * 动资产之前的全部校验：类能否解析、引脚名在不在、引用是否悬空、有没有环。
	 *
	 * 顺带做属性归一化（显式写出的默认值删掉），与导出方向的 delta 形态对齐——
	 * 校验看的是作者写了什么，归一化改的是拿去比较的形态，所以顺序是先校验后归一化。
	 */
	ASSETFORGE_API void ValidateGraph(
		FMaterialGraph& InOutGraph,
		TArray<FForgeDiagnostic>& OutDiagnostics,
		const FString& InFieldPath);
}
