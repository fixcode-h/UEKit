// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ForgeStyles.h"
#include "AssetForgeCompat.h"

#include "Core/ForgeJsonText.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ForgeStyles
{

namespace
{
	const TCHAR* const FieldProps = TEXT("props");
	const TCHAR* const FieldSlot = TEXT("slot");
	const TCHAR* const FieldComment = TEXT("//");

	/** 值是对象就取出来，否则空（不链式 ->：类型不对时 TryGetObject 给的是空指针）。 */
	TSharedPtr<FJsonObject> ObjectOf(const TSharedPtr<FJsonValue>& InValue)
	{
		if (!InValue.IsValid() || InValue->Type != EJson::Object)
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!InValue->TryGetObject(Object) || Object == nullptr || !Object->IsValid())
		{
			return nullptr;
		}
		return *Object;
	}

	TSharedPtr<FJsonValue> ValueOf(const TSharedPtr<FJsonObject>& InObject, const FString& InKey)
	{
		if (!InObject.IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FJsonValue>* Found = InObject->Values.Find(FForgeJsonKey(InKey));
		return Found != nullptr ? *Found : nullptr;
	}

	/**
	 * 对象里和 InKey 是「同一个字段」的原键。按 NormalizeKey 比：导入对属性名大小写、`b` 前缀都宽容，
	 * 样式写 `bIsVariable`、节点写 `IsVariable` 时必须认成同一个，否则两份都进 Props、谁生效看运气。
	 */
	bool FindSameKey(const TSharedPtr<FJsonObject>& InObject, const FString& InKey, FString& OutKey)
	{
		if (!InObject.IsValid())
		{
			return false;
		}
		const FString Wanted = ForgeJsonText::NormalizeKey(InKey);
		for (const auto& Pair : InObject->Values)
		{
			const FString Key(ASSETFORGE_JSON_KEY(Pair.Key));
			if (ForgeJsonText::NormalizeKey(Key) == Wanted)
			{
				OutKey = Key;
				return true;
			}
		}
		return false;
	}

	/** 两个值等不等价：按规范形态比（字段顺序、键写法、对象引用的两种写法都不算差别），和差异判定同一口径。 */
	bool ValuesEqual(const TSharedPtr<FJsonValue>& InLeft, const TSharedPtr<FJsonValue>& InRight)
	{
		if (!InLeft.IsValid() || !InRight.IsValid())
		{
			return InLeft.IsValid() == InRight.IsValid();
		}
		const TSharedPtr<FJsonObject> LeftHolder = MakeShared<FJsonObject>();
		LeftHolder->SetField(TEXT("v"), InLeft);
		const TSharedPtr<FJsonObject> RightHolder = MakeShared<FJsonObject>();
		RightHolder->SetField(TEXT("v"), InRight);
		return ForgeJsonText::ToCanonicalString(LeftHolder) == ForgeJsonText::ToCanonicalString(RightHolder);
	}

	/** 去掉 InOwn 里和 InStyle 相同的字段（对象逐字段递归），返回新对象；InOwn 空时返回空。 */
	TSharedPtr<FJsonObject> StripObject(const TSharedPtr<FJsonObject>& InOwn, const TSharedPtr<FJsonObject>& InStyle)
	{
		if (!InOwn.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		for (const auto& Pair : InOwn->Values)
		{
			const FString Key(ASSETFORGE_JSON_KEY(Pair.Key));
			FString StyleKey;
			if (!FindSameKey(InStyle, Key, StyleKey))
			{
				Result->SetField(Key, Pair.Value);
				continue;
			}

			const TSharedPtr<FJsonValue> StyleValue = ValueOf(InStyle, StyleKey);
			const TSharedPtr<FJsonObject> OwnChild = ObjectOf(Pair.Value);
			const TSharedPtr<FJsonObject> StyleChild = ObjectOf(StyleValue);
			if (OwnChild.IsValid() && StyleChild.IsValid())
			{
				// 结构体只留和样式不同的那几个字段；全相同就整个不写（展开时会从样式补回来）。
				const TSharedPtr<FJsonObject> Rest = StripObject(OwnChild, StyleChild);
				if (Rest.IsValid() && Rest->Values.Num() > 0)
				{
					Result->SetObjectField(Key, Rest);
				}
				continue;
			}

			if (!ValuesEqual(Pair.Value, StyleValue))
			{
				Result->SetField(Key, Pair.Value);
			}
		}
		return Result;
	}

	/**
	 * 按顺序合并节点引用的几个样式（后面的覆盖前面的）。InReportPath 非空时把不存在的样式名报成错误；
	 * 写回方向传空串，只是拿不到就算了（解析时已经报过）。
	 */
	bool ResolveStyles(const TSharedPtr<FJsonObject>& InStyles, const TArray<FString>& InNames, const FString& InReportPath, TSharedPtr<FJsonObject>& OutProps, TSharedPtr<FJsonObject>& OutSlot, TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		bool bOk = true;
		for (const FString& StyleName : InNames)
		{
			const TSharedPtr<FJsonObject>* StyleObject = nullptr;
			if (!InStyles.IsValid() || !InStyles->TryGetObjectField(StyleName, StyleObject) || StyleObject == nullptr || !StyleObject->IsValid())
			{
				if (!InReportPath.IsEmpty())
				{
					OutDiagnostics.Emplace(
						EForgeSeverity::Error,
						InReportPath,
						FString::Printf(TEXT("没有叫 `%s` 的样式：在文档顶层 styles 里定义它，或改成已有的样式名"), *StyleName));
				}
				bOk = false;
				continue;
			}

			const TSharedPtr<FJsonObject>* StyleProps = nullptr;
			if ((*StyleObject)->TryGetObjectField(FieldProps, StyleProps) && StyleProps != nullptr && StyleProps->IsValid())
			{
				OutProps = MergeObjects(OutProps, *StyleProps);
			}
			const TSharedPtr<FJsonObject>* StyleSlot = nullptr;
			if ((*StyleObject)->TryGetObjectField(FieldSlot, StyleSlot) && StyleSlot != nullptr && StyleSlot->IsValid())
			{
				OutSlot = MergeObjects(OutSlot, *StyleSlot);
			}
		}
		return bOk;
	}

	void CarryNode(const FForgeNode& InExisting, FForgeNode& InOutExported)
	{
		InOutExported.Styles = InExisting.Styles;
		for (FForgeNode& Child : InOutExported.Children)
		{
			const FForgeNode* Match = InExisting.Children.FindByPredicate([&Child](const FForgeNode& Candidate) { return Candidate.Name == Child.Name; });
			if (Match != nullptr)
			{
				CarryNode(*Match, Child);
			}
		}
	}
}

TSharedPtr<FJsonObject> MergeObjects(const TSharedPtr<FJsonObject>& InBase, const TSharedPtr<FJsonObject>& InOver)
{
	// 总是建新对象：样式对象被很多节点共用，就地改会让一个节点的覆盖漏到别的节点上。
	const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	if (InBase.IsValid())
	{
		for (const auto& Pair : InBase->Values)
		{
			Result->SetField(ASSETFORGE_JSON_KEY(Pair.Key), Pair.Value);
		}
	}
	if (!InOver.IsValid())
	{
		return Result;
	}

	for (const auto& Pair : InOver->Values)
	{
		const FString Key(ASSETFORGE_JSON_KEY(Pair.Key));
		FString ExistingKey;
		if (FindSameKey(Result, Key, ExistingKey))
		{
			const TSharedPtr<FJsonObject> BaseChild = ObjectOf(ValueOf(Result, ExistingKey));
			const TSharedPtr<FJsonObject> OverChild = ObjectOf(Pair.Value);
			Result->RemoveField(ExistingKey);
			if (BaseChild.IsValid() && OverChild.IsValid())
			{
				Result->SetObjectField(Key, MergeObjects(BaseChild, OverChild));
				continue;
			}
		}
		Result->SetField(Key, Pair.Value);
	}
	return Result;
}

bool ParseStyleRefs(const TSharedPtr<FJsonValue>& InValue, const FString& InPath, TArray<FString>& OutNames, TArray<FForgeDiagnostic>& OutDiagnostics)
{
	if (InValue.IsValid() && InValue->Type == EJson::String && !InValue->AsString().IsEmpty())
	{
		OutNames.Add(InValue->AsString());
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (InValue.IsValid() && InValue->Type == EJson::Array && InValue->TryGetArray(Items) && Items != nullptr && Items->Num() > 0)
	{
		bool bOk = true;
		for (int32 Index = 0; Index < Items->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Item = (*Items)[Index];
			if (!Item.IsValid() || Item->Type != EJson::String || Item->AsString().IsEmpty())
			{
				OutDiagnostics.Emplace(EForgeSeverity::Error, FString::Printf(TEXT("%s[%d]"), *InPath, Index), TEXT("样式名必须是非空字符串"));
				bOk = false;
				continue;
			}
			OutNames.Add(Item->AsString());
		}
		return bOk;
	}

	OutDiagnostics.Emplace(EForgeSeverity::Error, InPath, TEXT("style 写样式名（\"NavButton\"）或样式名数组（[\"Base\", \"NavButton\"]，后面的覆盖前面的）"));
	return false;
}

bool ValidateStyles(const TSharedPtr<FJsonObject>& InStyles, TArray<FForgeDiagnostic>& OutDiagnostics)
{
	if (!InStyles.IsValid())
	{
		return true;
	}

	bool bOk = true;
	for (const auto& StylePair : InStyles->Values)
	{
		const FString StyleName(ASSETFORGE_JSON_KEY(StylePair.Key));
		if (StyleName == FieldComment)
		{
			continue;
		}
		const FString StylePath = FString::Printf(TEXT("styles.%s"), *StyleName);
		const TSharedPtr<FJsonObject> Style = ObjectOf(StylePair.Value);
		if (!Style.IsValid())
		{
			OutDiagnostics.Emplace(EForgeSeverity::Error, StylePath, TEXT("样式必须是对象：{ \"props\": {…}, \"slot\": {…} }"));
			bOk = false;
			continue;
		}

		for (const auto& FieldPair : Style->Values)
		{
			const FString Field(ASSETFORGE_JSON_KEY(FieldPair.Key));
			if (Field == FieldComment)
			{
				continue;
			}
			if (Field != FieldProps && Field != FieldSlot)
			{
				OutDiagnostics.Emplace(
					EForgeSeverity::Error,
					FString::Printf(TEXT("%s.%s"), *StylePath, *Field),
					TEXT("样式只能写 props 和 slot；class、name、children 这些写在节点上"));
				bOk = false;
				continue;
			}
			if (!ObjectOf(FieldPair.Value).IsValid())
			{
				OutDiagnostics.Emplace(EForgeSeverity::Error, FString::Printf(TEXT("%s.%s"), *StylePath, *Field), TEXT("必须是 JSON 对象"));
				bOk = false;
			}
		}
	}
	return bOk;
}

bool ExpandTree(FForgeNode& InOutNode, const FString& InNodePath, const TSharedPtr<FJsonObject>& InStyles, TArray<FForgeDiagnostic>& OutDiagnostics)
{
	bool bOk = true;
	if (InOutNode.Styles.Num() > 0)
	{
		TSharedPtr<FJsonObject> StyleProps;
		TSharedPtr<FJsonObject> StyleSlot;
		if (ResolveStyles(InStyles, InOutNode.Styles, InNodePath + TEXT(".style"), StyleProps, StyleSlot, OutDiagnostics))
		{
			// 节点自己写的覆盖样式：样式在下、节点在上。
			if (StyleProps.IsValid() && StyleProps->Values.Num() > 0)
			{
				InOutNode.Props = MergeObjects(StyleProps, InOutNode.Props);
			}
			if (StyleSlot.IsValid() && StyleSlot->Values.Num() > 0)
			{
				InOutNode.Slot = MergeObjects(StyleSlot, InOutNode.Slot);
			}
		}
		else
		{
			bOk = false;
		}
	}

	for (int32 Index = 0; Index < InOutNode.Children.Num(); ++Index)
	{
		bOk &= ExpandTree(InOutNode.Children[Index], FString::Printf(TEXT("%s.children[%d]"), *InNodePath, Index), InStyles, OutDiagnostics);
	}
	return bOk;
}

FForgeNode StripTree(const FForgeNode& InNode, const TSharedPtr<FJsonObject>& InStyles)
{
	FForgeNode Out;
	Out.ClassName = InNode.ClassName;
	Out.Name = InNode.Name;
	Out.Ref = InNode.Ref;
	Out.Props = InNode.Props;
	Out.Slot = InNode.Slot;
	Out.Extra = InNode.Extra;
	Out.Styles = InNode.Styles;

	if (InNode.Styles.Num() > 0)
	{
		TSharedPtr<FJsonObject> StyleProps;
		TSharedPtr<FJsonObject> StyleSlot;
		TArray<FForgeDiagnostic> Ignored;
		if (ResolveStyles(InStyles, InNode.Styles, FString(), StyleProps, StyleSlot, Ignored))
		{
			Out.Props = StripObject(InNode.Props, StyleProps);
			Out.Slot = StripObject(InNode.Slot, StyleSlot);
		}
	}

	Out.Children.Reserve(InNode.Children.Num());
	for (const FForgeNode& Child : InNode.Children)
	{
		Out.Children.Add(StripTree(Child, InStyles));
	}
	return Out;
}

void CarryStyles(const FForgeDocument& InExisting, FForgeDocument& InOutExported)
{
	// 原配置没写 styles 就没有可保留的（节点上的 style 引用在解析时就要求 styles 里有定义）。
	if (!InExisting.Styles.IsValid())
	{
		return;
	}
	InOutExported.Styles = InExisting.Styles;
	CarryNode(InExisting.Root, InOutExported.Root);
}

} // namespace ForgeStyles
