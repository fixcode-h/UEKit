// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 算法层：目标节点树（配置）与当前节点树（资产）的差异计算。
 *
 * 匹配键是**控件名**而不是位置。UE 5.8 已删除 UWidget::WidgetGuid，控件身份由
 * UWidgetBlueprint::WidgetVariableNameToGuidMap 以「变量名 → GUID」持久化，
 * 编译器还会用 FGuid::NewDeterministicGuid(GetPathName()) 兜底——名字就是引擎认的稳定身份。
 * 按名匹配而不是清空重建，是为了保住引用、动画绑定与手工配置过的 Slate 属性。
 */
namespace ForgeDiff
{
	/**
	 * 计算计划。InNodePath 是当前节点自身的路径（根传 `root`），
	 * 递归时子节点拼成 `root/Canvas/HealthBar`。
	 */
	ASSETFORGE_API void ComputePlan(
		const FForgeNode& InTarget,
		const FForgeNode& InCurrent,
		const FString& InNodePath,
		TArray<FForgePlanEntry>& OutPlan);

	/** 便捷入口：以 `root` 为起点比较两棵树。 */
	ASSETFORGE_API void ComputeRootPlan(
		const FForgeNode& InTarget,
		const FForgeNode& InCurrent,
		TArray<FForgePlanEntry>& OutPlan);

	/**
	 * 两组属性之间的字段级差异描述，无差异返回 InPath 本身。
	 *
	 * 判定用的是规范形式（键大小写、`b` 前缀、数字精度都已被规范化），下钻到具体字段，
	 * 形如 `props.WaitTime.DefaultValue（配置 2 → 资产 3.5）`。材质图那种不走树模型的
	 * kind 自己算计划时复用它，免得再写一份判等规则。
	 */
	ASSETFORGE_API FString DescribeObjectDiff(
		const TSharedPtr<FJsonObject>& InTarget,
		const TSharedPtr<FJsonObject>& InCurrent,
		const FString& InPath);
}
