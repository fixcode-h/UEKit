// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ForgeDoc.h"

namespace ForgeDoc
{

namespace
{
	const TCHAR* const FieldKind = TEXT("kind");
	const TCHAR* const FieldSchemaVersion = TEXT("schemaVersion");
	const TCHAR* const FieldRevision = TEXT("revision");
	const TCHAR* const FieldAsset = TEXT("asset");
	const TCHAR* const FieldParent = TEXT("parent");
	const TCHAR* const FieldRoot = TEXT("root");
	const TCHAR* const FieldClass = TEXT("class");
	const TCHAR* const FieldName = TEXT("name");
	const TCHAR* const FieldRef = TEXT("ref");
	const TCHAR* const FieldProps = TEXT("props");
	const TCHAR* const FieldSlot = TEXT("slot");
	const TCHAR* const FieldChildren = TEXT("children");

	FString MakeChildPath(const FString& InParentPath, const FString& InField)
	{
		return InParentPath.IsEmpty() ? InField : FString::Printf(TEXT("%s.%s"), *InParentPath, *InField);
	}

	bool ReadRequiredString(
		const TSharedPtr<FJsonObject>& InJson,
		const TCHAR* InField,
		const FString& InPath,
		FString& OutValue,
		TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		if (InJson->TryGetStringField(InField, OutValue) && !OutValue.IsEmpty())
		{
			return true;
		}

		OutDiagnostics.Emplace(
			EForgeSeverity::Error,
			MakeChildPath(InPath, InField),
			FString::Printf(TEXT("缺少必需字段 `%s`，或它不是非空字符串"), InField));
		return false;
	}

	bool ReadRequiredInt(
		const TSharedPtr<FJsonObject>& InJson,
		const TCHAR* InField,
		const FString& InPath,
		int32& OutValue,
		TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		if (InJson->TryGetNumberField(InField, OutValue))
		{
			return true;
		}

		OutDiagnostics.Emplace(
			EForgeSeverity::Error,
			MakeChildPath(InPath, InField),
			FString::Printf(TEXT("缺少必需字段 `%s`，或它不是整数"), InField));
		return false;
	}
}

bool ParseNode(
	const TSharedPtr<FJsonObject>& InJson,
	const FString& InNodePath,
	FForgeNode& OutNode,
	TArray<FForgeDiagnostic>& OutDiagnostics)
{
	if (!InJson.IsValid())
	{
		OutDiagnostics.Emplace(EForgeSeverity::Error, InNodePath, TEXT("节点必须是 JSON 对象"));
		return false;
	}

	bool bOk = true;
	bOk &= ReadRequiredString(InJson, FieldName, InNodePath, OutNode.Name, OutDiagnostics);

	InJson->TryGetStringField(FieldRef, OutNode.Ref);

	if (InJson->HasField(FieldRef))
	{
		// 给了 ref 就可以省掉 class：那个类名由被引用的配置解析出来（见 ForgePipeline::ResolveRefs）。
		InJson->TryGetStringField(FieldClass, OutNode.ClassName);
	}
	else
	{
		bOk &= ReadRequiredString(InJson, FieldClass, InNodePath, OutNode.ClassName, OutDiagnostics);
	}

	if (const TSharedPtr<FJsonObject>* PropsObject = nullptr; InJson->TryGetObjectField(FieldProps, PropsObject))
	{
		OutNode.Props = *PropsObject;
	}

	if (const TSharedPtr<FJsonObject>* SlotObject = nullptr; InJson->TryGetObjectField(FieldSlot, SlotObject))
	{
		OutNode.Slot = *SlotObject;
	}

	if (const TArray<TSharedPtr<FJsonValue>>* ChildrenArray = nullptr; InJson->TryGetArrayField(FieldChildren, ChildrenArray))
	{
		int32 Index = 0;
		for (const TSharedPtr<FJsonValue>& ChildValue : *ChildrenArray)
		{
			const FString ChildPath = FString::Printf(TEXT("%s.children[%d]"), *InNodePath, Index);
			++Index;

			const TSharedPtr<FJsonObject>* ChildObject = nullptr;
			if (!ChildValue.IsValid() || !ChildValue->TryGetObject(ChildObject) || !ChildObject->IsValid())
			{
				OutDiagnostics.Emplace(EForgeSeverity::Error, ChildPath, TEXT("子节点必须是 JSON 对象"));
				bOk = false;
				continue;
			}

			FForgeNode Child;
			bOk &= ParseNode(*ChildObject, ChildPath, Child, OutDiagnostics);
			OutNode.Children.Add(MoveTemp(Child));
		}
	}

	// 未识别的字段收进 Extra：通用层只保证不丢，由各 kind 自己解释。
	if (const TSharedPtr<FJsonObject> Extra = MakeShared<FJsonObject>(); Extra.IsValid())
	{
		for (const TPair<UE::FSharedString, TSharedPtr<FJsonValue>>& Pair : InJson->Values)
		{
			const FString KeyString(Pair.Key.ToView());
			if (KeyString != FieldClass
				&& KeyString != FieldName
				&& KeyString != FieldRef
				&& KeyString != FieldProps
				&& KeyString != FieldSlot
				&& KeyString != FieldChildren)
			{
				Extra->SetField(KeyString, Pair.Value);
			}
		}

		if (Extra->Values.Num() > 0)
		{
			OutNode.Extra = Extra;
		}
	}

	// 兄弟重名只给警告，不拦：name 是不是匹配键由**各 kind 自己**决定。
	// UMG 用它做匹配键（重名必须报错，那边会再查一遍）；行为树的节点没有稳定名字，
	// 同级的两个 Wait 是常态，位置才是它的身份。
	TSet<FString> SeenNames;
	for (const FForgeNode& Child : OutNode.Children)
	{
		bool bAlreadyPresent = false;
		SeenNames.Add(Child.Name, &bAlreadyPresent);
		if (bAlreadyPresent)
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Warning,
				FString::Printf(TEXT("%s.children"), *InNodePath),
				FString::Printf(
					TEXT("同一父节点下出现重名 `%s`；若这个 kind 用 name 做匹配键就会匹配错，请确认它按位置匹配"),
					*Child.Name));
		}
	}

	return bOk;
}

TSharedPtr<FJsonObject> NodeToJson(const FForgeNode& InNode)
{
	const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(FieldClass, InNode.ClassName);
	Json->SetStringField(FieldName, InNode.Name);

	if (!InNode.Ref.IsEmpty())
	{
		Json->SetStringField(FieldRef, InNode.Ref);
	}

	if (InNode.Props.IsValid() && InNode.Props->Values.Num() > 0)
	{
		Json->SetObjectField(FieldProps, InNode.Props);
	}

	if (InNode.Slot.IsValid() && InNode.Slot->Values.Num() > 0)
	{
		Json->SetObjectField(FieldSlot, InNode.Slot);
	}

	if (InNode.Extra.IsValid())
	{
		for (const TPair<UE::FSharedString, TSharedPtr<FJsonValue>>& Pair : InNode.Extra->Values)
		{
			const FString KeyString(Pair.Key.ToView());
			if (!Json->HasField(KeyString))
			{
				Json->SetField(KeyString, Pair.Value);
			}
		}
	}

	if (InNode.Children.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ChildrenArray;
		ChildrenArray.Reserve(InNode.Children.Num());
		for (const FForgeNode& Child : InNode.Children)
		{
			ChildrenArray.Add(MakeShared<FJsonValueObject>(NodeToJson(Child)));
		}
		Json->SetArrayField(FieldChildren, ChildrenArray);
	}

	return Json;
}

bool Parse(
	const TSharedPtr<FJsonObject>& InJson,
	const FString& InSourcePath,
	FForgeDocument& OutDocument,
	TArray<FForgeDiagnostic>& OutDiagnostics)
{
	if (!InJson.IsValid())
	{
		OutDiagnostics.Emplace(EForgeSeverity::Error, TEXT(""), TEXT("配置文件的顶层必须是 JSON 对象"));
		return false;
	}

	OutDocument.SourcePath = InSourcePath;

	bool bOk = true;
	bOk &= ReadRequiredString(InJson, FieldKind, TEXT(""), OutDocument.Kind, OutDiagnostics);
	bOk &= ReadRequiredInt(InJson, FieldSchemaVersion, TEXT(""), OutDocument.SchemaVersion, OutDiagnostics);
	bOk &= ReadRequiredString(InJson, FieldAsset, TEXT(""), OutDocument.Asset, OutDiagnostics);

	// revision 与 parent 缺失不致命：revision 只用于警告，parent 在资产已存在时可由现有资产推断。
	InJson->TryGetNumberField(FieldRevision, OutDocument.Revision);
	InJson->TryGetStringField(FieldParent, OutDocument.Parent);

	if (!bOk)
	{
		return false;
	}

	if (OutDocument.SchemaVersion != CurrentSchemaVersion)
	{
		OutDiagnostics.Emplace(
			EForgeSeverity::Error,
			FieldSchemaVersion,
			FString::Printf(
				TEXT("schemaVersion=%d 不是本插件支持的版本（当前 %d）；升级插件或迁移配置，不要靠猜测继续"),
				OutDocument.SchemaVersion,
				CurrentSchemaVersion));
		return false;
	}

	const TSharedPtr<FJsonObject>* RootObject = nullptr;
	if (!InJson->TryGetObjectField(FieldRoot, RootObject) || !RootObject->IsValid())
	{
		OutDiagnostics.Emplace(EForgeSeverity::Error, FieldRoot, TEXT("缺少必需字段 `root`，或它不是 JSON 对象"));
		return false;
	}

	bOk &= ParseNode(*RootObject, FieldRoot, OutDocument.Root, OutDiagnostics);

	// 未识别字段原样留着：回写时不丢信息，也便于将来加字段时向前兼容。
	const TSharedPtr<FJsonObject> Extra = MakeShared<FJsonObject>();
	for (const TPair<UE::FSharedString, TSharedPtr<FJsonValue>>& Pair : InJson->Values)
	{
		// UE 5.8 的 FJsonObject::Values 以 UE::FSharedString 为键，跨到 FString 才能比较与写回。
		const FString KeyString(Pair.Key.ToView());
		if (KeyString != FieldKind
			&& KeyString != FieldSchemaVersion
			&& KeyString != FieldRevision
			&& KeyString != FieldAsset
			&& KeyString != FieldParent
			&& KeyString != FieldRoot)
		{
			Extra->SetField(KeyString, Pair.Value);
		}
	}
	if (Extra->Values.Num() > 0)
	{
		OutDocument.Extra = Extra;
	}

	return bOk;
}

TSharedPtr<FJsonObject> ToJson(const FForgeDocument& InDocument)
{
	const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(FieldKind, InDocument.Kind);
	Json->SetNumberField(FieldSchemaVersion, InDocument.SchemaVersion);
	Json->SetNumberField(FieldRevision, InDocument.Revision);
	Json->SetStringField(FieldAsset, InDocument.Asset);

	if (!InDocument.Parent.IsEmpty())
	{
		Json->SetStringField(FieldParent, InDocument.Parent);
	}

	Json->SetObjectField(FieldRoot, NodeToJson(InDocument.Root));

	if (InDocument.Extra.IsValid())
	{
		for (const TPair<UE::FSharedString, TSharedPtr<FJsonValue>>& Pair : InDocument.Extra->Values)
		{
			const FString KeyString(Pair.Key.ToView());
			if (!Json->HasField(KeyString))
			{
				Json->SetField(KeyString, Pair.Value);
			}
		}
	}

	return Json;
}

} // namespace ForgeDoc
