// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

class UObject;

/**
 * 适配层：UObject 的反射属性 ↔ JSON 值。
 *
 * 这是「编辑器里能配的，配置里就能配」的落点：不逐个控件手写字段表，而是遍历
 * FProperty 并筛出 CPF_Edit 的那些，交给引擎自带的 FJsonObjectConverter 做值转换。
 * 于是插槽、渲染、渲染变换这些属性**全都不用单独写代码**。
 *
 * 两处刻意的取舍：
 * - 导出只写「与 CDO 不同」的属性（delta）。配置因此干净稳定，引擎改默认值时也不会整片抖动。
 * - 名字用 UE 属性名原样（`RenderOpacity` 而不是 `renderOpacity`）。零映射成本，
 *   反射直接查得到，AI 查引擎文档能对上，导出再导入不漂移。
 */
namespace ForgePropertyCodec
{
	/** 该属性是否走配置通道。排除瞬态、Instanced 子对象（它们各有自己的通道）。 */
	ASSETFORGE_API bool IsCodecProperty(const FProperty* InProperty);

	/**
	 * 按配置里的名字找属性。
	 *
	 * 除了精确匹配，还接受去掉 `b` 前缀的写法（`isEnabled` → `bIsEnabled`）——
	 * UMG 的布尔属性大量带 `b` 前缀，让 AI 去记这一点是没必要的负担。
	 */
	ASSETFORGE_API FProperty* FindCodecProperty(const UStruct* InStruct, const FString& InName);

	/**
	 * 只校验不写入：JSON 里的每个字段都要能落到这个类型上，结构体递归检查。
	 *
	 * 为什么需要它：`FJsonObjectConverter` 的严格模式要求结构体字段写全（`FMargin` 缺一个
	 * `Left` 就报错），而只写想改的字段才是配置该有的用法。所以关掉严格模式，
	 * 由这道校验补回「字段名写错要能发现」——而且它在**动资产之前**跑，不会留下改到一半的资产。
	 *
	 * 结构体内部的字段只查「名字存在」：结构体是整体导出的（`UPropertyToJsonValue` 不按
	 * `CPF_Edit` 过滤内部字段），而引擎的结构体里本来就有大量无 `Edit` 的内部/废弃字段——
	 * 要求它们可配会让「导出 → 导入」永远不收敛（`FLightmassWorldInfoSettings::EmissiveBoost`
	 * 就是这样撞上的）。顶层字段仍然要求可配。
	 */
	ASSETFORGE_API void ValidateProperties(const UStruct* InStruct, const TSharedPtr<FJsonObject>& InJson, TArray<FForgeDiagnostic>& OutDiagnostics, const FString& InFieldPath, bool bInStructRecursion = false);

	/**
	 * 把对象上所有走配置通道的属性重置成 CDO 的值。
	 *
	 * 这是「配置是唯一真源」的落点：配置没写的东西，资产里就不该残留——否则资产会带着
	 * 一份配置不知道的状态，而导出方向是 delta 的，那份状态既导不出来也说不清从哪来。
	 */
	ASSETFORGE_API void ResetToDefaults(UObject* InObject);

	/**
	 * 递归删掉与默认值相同的字段。
	 *
	 * 导出方向是 delta 的（等于默认值的属性不写），而配置由人/AI 手写——顺手把默认值
	 * 也写出来是很自然的。不归一化的话，同一个状态会因为「写没写那个默认值」而被判成不同，
	 * 每次导入都误报一遍「已更新」。归一化之后「显式写默认值」与「省略」完全等价。
	 */
	ASSETFORGE_API void PruneDefaults(const UStruct* InStruct, const TSharedPtr<FJsonObject>& InOutJson);

	/**
	 * 把 float 写成「转回 float 后仍相同」的最短十进制 JSON 值。
	 *
	 * 0.6f 直转 double 是 0.6000000238418579——写进配置就是噪声，与手写的 0.6 判不相等，
	 * 每次导入都误报差异。反射属性的导出已内置这层归一（走 `ShortenFloatFields`）；
	 * **直接手写 JSON 的处理器**（如 Niagara 的可写参数导出）要对每个 float 值调用它。
	 */
	ASSETFORGE_API TSharedPtr<FJsonValue> ShortenFloatValue(float InValue);

	// ---- 内存版：直接对「结构体 + 数据指针」做编解码 ----
	// DataTable 的一行就是这种形态（RowStruct + uint8*），它不是 UObject，走不了下面那组包装。

	/**
	 * 把与默认数据不同的可编辑属性写进 OutJson。InDefaultData 可为 nullptr（那样就写全量）。
	 * 注意：一个属性都没写出来时 OutJson 会被置回 nullptr（哪怕调用前已经 MakeShared 过），调用方用之前必须判 IsValid()。
	 */
	ASSETFORGE_API void ExportStructDelta(const UStruct* InStruct, const void* InData, const void* InDefaultData, TSharedPtr<FJsonObject>& OutJson, TArray<FForgeDiagnostic>& OutDiagnostics, const FString& InFieldPath, const TArray<FName>* InExcludedPropertyNames = nullptr);

	/**
	 * 把 JSON 里的属性写进一块结构体数据。
	 *
	 * `InOwner` 是宿主对象（可为空——DataTable 的行不是 UObject）：写「元素带 `Instanced`
	 * 的对象数组」（GE 的 `GEComponents`、输入映射的修饰器这类）时，新建的元素要以它做 Outer。
	 * 那种数组交给 `FJsonObjectConverter` 会撞引擎的 `ClassWithin` 检查，见实现里的说明。
	 */
	ASSETFORGE_API void ImportStructProperties(const UStruct* InStruct, void* InData, const TSharedPtr<FJsonObject>& InJson, TArray<FForgeDiagnostic>& OutDiagnostics, const FString& InFieldPath, UObject* InOwner = nullptr);

	/** 把一块结构体数据重置成默认值（结构体的默认实例或全零，取引擎给的那个）。 */
	ASSETFORGE_API void ResetStructToDefaults(const UStruct* InStruct, void* InData);

	// ---- UObject 版：下面的都是包装 ----

	/**
	 * 把与 CDO 不同的可编辑属性写进 OutJson。
	 *
	 * `InExcludedPropertyNames` 是 kind 级的排除名单（如 `UFont::CompositeFont`——它由
	 * 配置的 children 表达，且结构内部含 FName，走 JSON 通道只会每次导出产生一条"无法转换"
	 * 的警告）。排除项不导出、不产生诊断；导入侧仍由各 kind 自己明确拒绝。
	 */
	ASSETFORGE_API void ExportDeltaProperties(const UObject* InObject, TSharedPtr<FJsonObject>& OutJson, TArray<FForgeDiagnostic>& OutDiagnostics, const FString& InFieldPath, const TArray<FName>* InExcludedPropertyNames = nullptr);

	/**
	 * 强制导出指定的属性：哪怕值等于默认，也写进 JSON（值取自 InObject）。
	 *
	 * 给「关键参数」用——纹理的压缩/寻址/mip 这类设置是配置读者要一眼看到、要能直接改的，
	 * 藏进 delta 省略里对读者不友好。名字按 `FindCodecProperty` 解析（带 `b` 前缀的容错）。
	 */
	ASSETFORGE_API void ExportForcedProperties(const UObject* InObject, const TArray<FName>& InPropertyNames, TSharedPtr<FJsonObject>& InOutJson, TArray<FForgeDiagnostic>& OutDiagnostics, const FString& InFieldPath);

	/** 把 JSON 里的属性写进对象；单个属性失败不影响其余属性。 */
	ASSETFORGE_API void ImportProperties(UObject* InObject, const TSharedPtr<FJsonObject>& InJson, TArray<FForgeDiagnostic>& OutDiagnostics, const FString& InFieldPath);
}
