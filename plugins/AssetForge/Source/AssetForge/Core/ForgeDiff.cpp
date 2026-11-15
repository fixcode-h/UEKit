// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ForgeDiff.h"
#include "Core/ForgeJsonText.h"

namespace ForgeDiff
{

namespace
{
	FString JoinPath(const FString& InParentPath, const FString& InName)
	{
		return InParentPath.IsEmpty() ? InName : FString::Printf(TEXT("%s/%s"), *InParentPath, *InName);
	}

	/**
	 * 单个 JSON 值没有直接的相等比较，包一层再序列化是可靠且短的做法。
	 *
	 * 用**规范形式**（排序 + 键规范化）而不是紧凑形式：诊断与判等必须走同一套规则，
	 * 否则会报出一堆「大小写不同」「字段顺序不同」的假差异。
	 */
	FString ValueToString(const TSharedPtr<FJsonValue>& InValue)
	{
		const TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
		Wrapper->SetField(TEXT("v"), InValue);
		return ForgeJsonText::ToCanonicalString(Wrapper);
	}

	/**
	 * 类名的比较形式。
	 *
	 * 同一个类有多种写法：生成类名带不带 `_C`（导出写 `GetPathName()`，含 `_C`；人工手写常省略）。
	 * 只归一这一条，然后逐字节比较——**刻意不做**"短名 vs 全路径"的归一：那会把不同包里的
	 * 同名类当成同一个，属于静默漏更新，比多报一次计划危险得多。
	 */
	FString ClassNameForCompare(const FString& InClassName)
	{
		return InClassName.EndsWith(TEXT("_C")) ? InClassName.LeftChop(2) : InClassName;
	}

	/** 值的简短表示，够看清差异即可（对象/数组最多取前 60 个字符）。 */
	FString ValueToDiagnosticText(const TSharedPtr<FJsonValue>& InValue)
	{
		if (!InValue.IsValid())
		{
			return TEXT("null");
		}

		switch (InValue->Type)
		{
		case EJson::String:
			return InValue->AsString();
		case EJson::Number:
			return FString::Printf(TEXT("%g"), InValue->AsNumber());
		case EJson::Boolean:
			return InValue->AsBool() ? TEXT("true") : TEXT("false");
		default:
			return ValueToString(InValue).Left(60);
		}
	}

	/** 按规范化后的键建索引：与判等用同一套键规则。 */
	void IndexByNormalizedKey(
		const TSharedPtr<FJsonObject>& InObject,
		TMap<FString, TSharedPtr<FJsonValue>>& OutValues,
		TMap<FString, FString>& OutDisplayKeys)
	{
		if (!InObject.IsValid())
		{
			return;
		}

		for (const TPair<UE::FSharedString, TSharedPtr<FJsonValue>>& Pair : InObject->Values)
		{
			const FString Key(Pair.Key.ToView());
			const FString Normalized = ForgeJsonText::NormalizeKey(Key);
			OutValues.Add(Normalized, Pair.Value);
			OutDisplayKeys.Add(Normalized, Key);
		}
	}

	/**
	 * 收集「哪里不一样」的字段路径，能下钻就下钻。
	 *
	 * 只说到 `props: WaitTime` 是定位不了问题的——`WaitTime` 里还有 `defaultValue` 与 `key`。
	 * 两边都是对象时继续往下走一层，直到某个具体字段或数组。
	 */
	void CollectChangedPaths(
		const TSharedPtr<FJsonObject>& InTarget,
		const TSharedPtr<FJsonObject>& InCurrent,
		const FString& InPath,
		TArray<FString>& OutPaths)
	{
		TMap<FString, TSharedPtr<FJsonValue>> TargetValues;
		TMap<FString, TSharedPtr<FJsonValue>> CurrentValues;
		TMap<FString, FString> TargetKeys;
		TMap<FString, FString> CurrentKeys;
		IndexByNormalizedKey(InTarget, TargetValues, TargetKeys);
		IndexByNormalizedKey(InCurrent, CurrentValues, CurrentKeys);

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : TargetValues)
		{
			const FString Key = TargetKeys[Pair.Key];
			const TSharedPtr<FJsonValue>* CurrentValue = CurrentValues.Find(Pair.Key);

			if (CurrentValue == nullptr)
			{
				OutPaths.Add(FString::Printf(TEXT("%s.%s（仅配置）"), *InPath, *Key));
				continue;
			}

			if (ValueToString(Pair.Value) == ValueToString(*CurrentValue))
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* TargetChild = nullptr;
			const TSharedPtr<FJsonObject>* CurrentChild = nullptr;
			if (Pair.Value->TryGetObject(TargetChild) && (*CurrentValue)->TryGetObject(CurrentChild))
			{
				CollectChangedPaths(*TargetChild, *CurrentChild, FString::Printf(TEXT("%s.%s"), *InPath, *Key), OutPaths);
			}
			else
			{
				OutPaths.Add(FString::Printf(
					TEXT("%s.%s（配置 %s → 资产 %s）"),
					*InPath,
					*Key,
					*ValueToDiagnosticText(Pair.Value),
					*ValueToDiagnosticText(*CurrentValue)));
			}
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : CurrentValues)
		{
			if (!TargetValues.Contains(Pair.Key))
			{
				OutPaths.Add(FString::Printf(TEXT("%s.%s（仅资产）"), *InPath, *CurrentKeys[Pair.Key]));
			}
		}
	}

}

/** 列出变化了的字段——报告里直接给字段比给整块 JSON 更有用（材质图的计划也复用它）。 */
FString DescribeObjectDiff(
	const TSharedPtr<FJsonObject>& InTarget,
	const TSharedPtr<FJsonObject>& InCurrent,
	const FString& InPath)
{
	TArray<FString> Paths;
	CollectChangedPaths(InTarget, InCurrent, InPath, Paths);

	if (Paths.Num() == 0)
	{
		return InPath;
	}

	Paths.Sort();
	return FString::Join(Paths, TEXT(", "));
}

namespace
{

	/**
	 * 两个属性集合是否等价。
	 *
	 * 「没有这个集合」与「空的集合」必须视为相同：前者是配置里没写 props，
	 * 后者是导出时没导出任何属性——它们表达的是同一件事，若判为不同，
	 * 每次回环都会把每个节点误报成「已更新」。
	 */
	bool AreJsonObjectsEquivalent(const TSharedPtr<FJsonObject>& InLeft, const TSharedPtr<FJsonObject>& InRight)
	{
		const bool bLeftEmpty = !InLeft.IsValid() || InLeft->Values.Num() == 0;
		const bool bRightEmpty = !InRight.IsValid() || InRight->Values.Num() == 0;
		if (bLeftEmpty && bRightEmpty)
		{
			return true;
		}

		// 用规范形式比：字段顺序由「谁写的」决定，不该影响相等判断。
		return ForgeJsonText::ToCanonicalString(InLeft) == ForgeJsonText::ToCanonicalString(InRight);
	}

	/** 比较节点自身的内容（不含 children，children 由递归处理）。 */
	bool AreContentsEqual(const FForgeNode& InTarget, const FForgeNode& InCurrent, FString& OutDetail)
	{
		// 类名先归一：`..._C` 的带与不带指同一个类，逐字节比会每次回环都报「已更新」。
		if (ClassNameForCompare(InTarget.ClassName) != ClassNameForCompare(InCurrent.ClassName))
		{
			OutDetail = FString::Printf(TEXT("class: %s → %s"), *InCurrent.ClassName, *InTarget.ClassName);
			return false;
		}

		// ref 不参与比较：它只是 class 的推导来源，已经反映在 ClassName 里。
		// 而导出方向永远不写 ref（导出的是 class 本身），拿它比会让每个引用子界面的节点
		// 每次都被误报成「已更新」。

		if (!AreJsonObjectsEquivalent(InTarget.Props, InCurrent.Props))
		{
			OutDetail = DescribeObjectDiff(InTarget.Props, InCurrent.Props, TEXT("props"));
			return false;
		}

		if (!AreJsonObjectsEquivalent(InTarget.Slot, InCurrent.Slot))
		{
			OutDetail = DescribeObjectDiff(InTarget.Slot, InCurrent.Slot, TEXT("slot"));
			return false;
		}

		return true;
	}

	void AddCreateSubtree(const FForgeNode& InNode, const FString& InNodePath, TArray<FForgePlanEntry>& OutPlan)
	{
		OutPlan.Add({ EForgeAction::Create, InNodePath, InNode.Name, FString() });

		for (const FForgeNode& Child : InNode.Children)
		{
			AddCreateSubtree(Child, JoinPath(InNodePath, Child.Name), OutPlan);
		}
	}
}

void ComputePlan(
	const FForgeNode& InTarget,
	const FForgeNode& InCurrent,
	const FString& InNodePath,
	TArray<FForgePlanEntry>& OutPlan)
{
	FString Detail;
	if (!AreContentsEqual(InTarget, InCurrent, Detail))
	{
		OutPlan.Add({ EForgeAction::Update, InNodePath, InTarget.Name, MoveTemp(Detail) });
	}

	TMap<FString, const FForgeNode*> CurrentByName;
	CurrentByName.Reserve(InCurrent.Children.Num());
	for (const FForgeNode& Child : InCurrent.Children)
	{
		CurrentByName.Add(Child.Name, &Child);
	}

	TSet<FString> TargetNames;
	TargetNames.Reserve(InTarget.Children.Num());
	TArray<FString> TargetOrder;
	TargetOrder.Reserve(InTarget.Children.Num());

	for (const FForgeNode& Child : InTarget.Children)
	{
		TargetNames.Add(Child.Name);
		TargetOrder.Add(Child.Name);

		const FString ChildPath = JoinPath(InNodePath, Child.Name);
		if (const FForgeNode* const* Existing = CurrentByName.Find(Child.Name))
		{
			ComputePlan(Child, **Existing, ChildPath, OutPlan);
		}
		else
		{
			AddCreateSubtree(Child, ChildPath, OutPlan);
		}
	}

	for (const FForgeNode& Child : InCurrent.Children)
	{
		if (!TargetNames.Contains(Child.Name))
		{
			OutPlan.Add({
				EForgeAction::Delete,
				JoinPath(InNodePath, Child.Name),
				Child.Name,
				TEXT("配置中已移除") });
		}
	}

	// 顺序只在**双方都有**的节点之间比较：新增与删除已经各自记过一条，混进来会重复报。
	TArray<FString> CurrentOrder;
	CurrentOrder.Reserve(InCurrent.Children.Num());
	for (const FForgeNode& Child : InCurrent.Children)
	{
		if (TargetNames.Contains(Child.Name))
		{
			CurrentOrder.Add(Child.Name);
		}
	}

	if (CurrentOrder != TargetOrder)
	{
		OutPlan.Add({
			EForgeAction::Reorder,
			InNodePath,
			InTarget.Name,
			FString::Printf(
				TEXT("子节点顺序: [%s] → [%s]"),
				*FString::Join(CurrentOrder, TEXT(", ")),
				*FString::Join(TargetOrder, TEXT(", "))) });
	}
}

void ComputeRootPlan(
	const FForgeNode& InTarget,
	const FForgeNode& InCurrent,
	TArray<FForgePlanEntry>& OutPlan)
{
	ComputePlan(InTarget, InCurrent, TEXT("root"), OutPlan);
}

} // namespace ForgeDiff
