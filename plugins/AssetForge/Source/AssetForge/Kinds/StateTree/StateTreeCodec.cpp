// Copyright Epic Games, Inc. All Rights Reserved.

// 状态树只在 UE 5.8 上编（ASSETFORGE_WITH_STATETREE，见 AssetForge.Build.cs）：它依赖 5.6 才有的 PropertyBindingUtils，
// 5.4 的状态树绑定接口差别太大，这一类资产在 5.4 上先不支持。
#if ASSETFORGE_WITH_STATETREE

#include "Kinds/StateTree/StateTreeCodec.h"
#include "AssetForgeCompat.h"

#include "Core/ForgeDoc.h"
#include "Core/ForgePropertyCodec.h"

#include "Blueprint/StateTreeConditionBlueprintBase.h"
#include "Blueprint/StateTreeConsiderationBlueprintBase.h"
#include "Blueprint/StateTreeEvaluatorBlueprintBase.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "GameplayTagContainer.h"
#include "JsonObjectConverter.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEditorPropertyBindings.h"
#include "StateTreePropertyBindings.h"
#include "StateTreeState.h"
#include "StateTreeTasksStatus.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/UObjectGlobals.h"

namespace StateTreeCodec
{

namespace
{
	/** 枚举值 → 名字（解析器按名读，导出也按名写，两边同一套规则）。 */
	template <typename TEnum>
	FString EnumToString(const TEnum InValue)
	{
		if (const UEnum* Enum = StaticEnum<TEnum>())
		{
			return Enum->GetNameStringByValue(static_cast<int64>(InValue));
		}
		return FString();
	}

	// ---- 节点（FStateTreeEditorNode）-----

	/** 蓝图 wrapper 的类引用字段名；不是 wrapper 时返回 nullptr。 */
	const TCHAR* GetBlueprintWrapperClassField(const UScriptStruct* InNodeStruct)
	{
		if (InNodeStruct == nullptr)
		{
			return nullptr;
		}

		if (InNodeStruct->IsChildOf(TBaseStructure<FStateTreeBlueprintTaskWrapper>::Get()))
		{
			return TEXT("TaskClass");
		}
		if (InNodeStruct->IsChildOf(TBaseStructure<FStateTreeBlueprintEvaluatorWrapper>::Get()))
		{
			return TEXT("EvaluatorClass");
		}
		if (InNodeStruct->IsChildOf(TBaseStructure<FStateTreeBlueprintConditionWrapper>::Get()))
		{
			return TEXT("ConditionClass");
		}
		if (InNodeStruct->IsChildOf(TBaseStructure<FStateTreeBlueprintConsiderationWrapper>::Get()))
		{
			return TEXT("ConsiderationClass");
		}

		return nullptr;
	}

	/** 读蓝图 wrapper 引用的节点类；不是蓝图节点时返回 nullptr。 */
	UClass* GetBlueprintNodeClass(const FStateTreeEditorNode& InNode)
	{
		const UScriptStruct* NodeStruct = InNode.GetNode().GetScriptStruct();
		const TCHAR* ClassField = GetBlueprintWrapperClassField(NodeStruct);
		if (ClassField == nullptr)
		{
			return nullptr;
		}

		if (const FClassProperty* ClassProperty = FindFProperty<FClassProperty>(NodeStruct, ClassField))
		{
			return Cast<UClass>(ClassProperty->GetObjectPropertyValue_InContainer(InNode.GetNode().GetMemory()));
		}

		return nullptr;
	}

	/** 配置里的 type：蓝图节点写节点类，C++ 节点写节点结构。 */
	FString GetNodeTypePath(const FStateTreeEditorNode& InNode)
	{
		if (UClass* BlueprintClass = GetBlueprintNodeClass(InNode))
		{
			return BlueprintClass->GetPathName();
		}

		if (const UScriptStruct* NodeStruct = InNode.GetNode().GetScriptStruct())
		{
			return NodeStruct->GetPathName();
		}

		return FString();
	}

	/** 节点属性（实例值层 delta）；`Name` 由顶层字段承载，从 props 里排除。 */
	void ExportNodeProps(
		const FStateTreeEditorNode& InNode,
		TSharedPtr<FJsonObject>& OutProps,
		TArray<FForgeDiagnostic>& OutDiagnostics,
		const FString& InFieldPath)
	{
		const FStateTreeDataView Instance = InNode.GetInstance();
		if (!Instance.IsValid())
		{
			return;
		}

		if (const UScriptStruct* StructType = Cast<const UScriptStruct>(Instance.GetStruct()))
		{
			// delta 的基准是「同类型的默认构造实例」——与「配置没写就回默认」的语义一致。
			FInstancedStruct DefaultInstance;
			DefaultInstance.InitializeAs(StructType);
			ForgePropertyCodec::ExportStructDelta(
				StructType,
				Instance.GetMemory(),
				DefaultInstance.GetMemory(),
				OutProps,
				OutDiagnostics,
				InFieldPath);
		}
		else if (const UObject* Object = Instance.GetPtr<const UObject>())
		{
			// 蓝图节点：实例对象是 UObject，走 UObject 版 delta（对比类默认值）。
			ForgePropertyCodec::ExportDeltaProperties(Object, OutProps, OutDiagnostics, InFieldPath);
		}

		if (OutProps.IsValid())
		{
			OutProps->RemoveField(Field::Name);
		}
	}

	/** 一组编辑节点 → JSON 数组。 */
	TArray<TSharedPtr<FJsonValue>> ReadEditorNodes(
		TConstArrayView<FStateTreeEditorNode> InNodes,
		TArray<FForgeDiagnostic>& OutDiagnostics,
		const FString& InFieldPath)
	{
		TArray<TSharedPtr<FJsonValue>> Result;

		for (int32 Index = 0; Index < InNodes.Num(); ++Index)
		{
			const FStateTreeEditorNode& Node = InNodes[Index];
			const FString NodePath = FString::Printf(TEXT("%s[%d]"), *InFieldPath, Index);

			const FString TypePath = GetNodeTypePath(Node);
			if (TypePath.IsEmpty())
			{
				OutDiagnostics.Emplace(
					EForgeSeverity::Warning,
					NodePath,
					TEXT("节点未初始化（没有类型），导出时被跳过"));
				continue;
			}

			const TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
			NodeJson->SetStringField(Field::Type, TypePath);

			// 只写「显式起的名字」；未命名节点的默认显示名写进配置没有信息量。
			if (const FStateTreeNodeBase* NodeBase = Node.GetNode().GetPtr<FStateTreeNodeBase>())
			{
				if (!NodeBase->Name.IsNone())
				{
					NodeJson->SetStringField(Field::Name, NodeBase->Name.ToString());
				}
			}

			TSharedPtr<FJsonObject> Props;
			ExportNodeProps(Node, Props, OutDiagnostics, NodePath);
			if (Props.IsValid() && Props->Values.Num() > 0)
			{
				NodeJson->SetObjectField(Field::Props, Props);
			}

			Result.Add(MakeShared<FJsonValueObject>(NodeJson));
		}

		return Result;
	}

	// ---- 转移 ----

	/** 状态链接的引用文本：GotoState 用状态路径（按 ID 反查），其余用类型名。 */
	FString DescribeStateLink(const FStateTreeStateLink& InLink, const UStateTreeState* InSelfState)
	{
		if (InLink.LinkType == EStateTreeTransitionType::GotoState)
		{
			// 按 ID 反查目标状态——名字可能重复，ID 才是身份。
			if (const UStateTree* Tree = InSelfState ? InSelfState->GetTypedOuter<UStateTree>() : nullptr)
			{
				if (const UStateTreeEditorData* EditorData = Cast<const UStateTreeEditorData>(Tree->EditorData.Get()))
				{
					if (const UStateTreeState* Target = EditorData->GetStateByID(InLink.ID))
					{
						return Target->GetPath();
					}
				}
			}

			return InLink.Name.ToString();
		}

		return EnumToString(InLink.LinkType);
	}

	/** 事件描述（RequiredEvent / RequiredEventToEnter 共用）。 */
	void WriteEventDesc(const FStateTreeEventDesc& InEvent, TSharedPtr<FJsonObject>& OutJson)
	{
		if (InEvent.Tag.IsValid())
		{
			OutJson->SetStringField(Field::Tag, InEvent.Tag.ToString());
		}
		if (InEvent.PayloadStruct != nullptr)
		{
			OutJson->SetStringField(Field::Payload, InEvent.PayloadStruct->GetPathName());
		}
		if (!InEvent.bConsumeEventOnSelect)
		{
			OutJson->SetBoolField(Field::Consume, false);
		}
	}

	TSharedPtr<FJsonObject> ReadTransition(
		const FStateTreeTransition& InTransition,
		const UStateTreeState& InState,
		TArray<FForgeDiagnostic>& OutDiagnostics,
		const FString& InFieldPath)
	{
		const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

		Json->SetStringField(Field::Trigger, EnumToString(InTransition.Trigger));

		if (InTransition.RequiredEvent.Tag.IsValid() || InTransition.RequiredEvent.PayloadStruct != nullptr
			|| !InTransition.RequiredEvent.bConsumeEventOnSelect)
		{
			TSharedPtr<FJsonObject> EventJson = MakeShared<FJsonObject>();
			WriteEventDesc(InTransition.RequiredEvent, EventJson);
			Json->SetObjectField(Field::Event, EventJson);
		}

		Json->SetStringField(Field::To, DescribeStateLink(InTransition.State, &InState));

		if (InTransition.Priority != EStateTreeTransitionPriority::Normal)
		{
			Json->SetStringField(Field::Priority, EnumToString(InTransition.Priority));
		}
		if (InTransition.bDelayTransition)
		{
			Json->SetNumberField(Field::Delay, InTransition.DelayDuration);
			if (InTransition.DelayRandomVariance != 0.0f)
			{
				Json->SetNumberField(Field::DelayVariance, InTransition.DelayRandomVariance);
			}
		}
		if (!InTransition.bTransitionEnabled)
		{
			Json->SetBoolField(Field::Enabled, false);
		}
		if (InTransition.ReactivateTargetState != EStateTreeTransitionChangeTypeRules::Default)
		{
			Json->SetStringField(TEXT("reactivateTargetState"), EnumToString(InTransition.ReactivateTargetState));
		}
		if (InTransition.Conditions.Num() > 0)
		{
			Json->SetArrayField(
				Field::Conditions,
				ReadEditorNodes(InTransition.Conditions, OutDiagnostics, InFieldPath + TEXT(".conditions")));
		}

		return Json;
	}

	// ---- 状态 ----

	void ReadStateInto(
		const UStateTreeState& InState,
		FForgeNode& OutNode,
		TArray<FForgeDiagnostic>& OutDiagnostics,
		const FString& InFieldPath)
	{
		OutNode.Name = InState.Name.ToString();
		// 契约层要求每个节点有 `class`；状态对象本身就是 `UStateTreeState`，写它的类路径（固定值）。
		OutNode.ClassName = UStateTreeState::StaticClass()->GetPathName();

		const TSharedPtr<FJsonObject> Extra = MakeShared<FJsonObject>();

		if (InState.Type != EStateTreeStateType::State)
		{
			Extra->SetStringField(Field::Type, EnumToString(InState.Type));
		}
		if (InState.SelectionBehavior != EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder)
		{
			Extra->SetStringField(Field::Selection, EnumToString(InState.SelectionBehavior));
		}
		if (InState.TasksCompletion != EStateTreeTaskCompletionType::Any)
		{
			Extra->SetStringField(Field::TasksCompletion, EnumToString(InState.TasksCompletion));
		}
		if (InState.Tag.IsValid())
		{
			Extra->SetStringField(Field::Tag, InState.Tag.ToString());
		}
		if (!InState.Description.IsEmpty())
		{
			Extra->SetStringField(Field::Description, InState.Description);
		}
		if (!FMath::IsNearlyEqual(InState.Weight, 1.0f))
		{
			Extra->SetNumberField(Field::Weight, InState.Weight);
		}
		if (!InState.bEnabled)
		{
			Extra->SetBoolField(Field::Enabled, false);
		}
		if (InState.bHasCustomTickRate)
		{
			Extra->SetNumberField(Field::CustomTickRate, InState.CustomTickRate);
		}
		if (InState.bCopyParameterBindingsOnTick)
		{
			Extra->SetBoolField(Field::CopyParameterBindingsOnTick, true);
		}
		if (!InState.bCheckPrerequisitesWhenActivatingChildDirectly)
		{
			Extra->SetBoolField(Field::CheckPrerequisites, false);
		}
		if (InState.LinkedAsset != nullptr)
		{
			Extra->SetStringField(Field::LinkedAsset, InState.LinkedAsset->GetPathName());
		}
		if (InState.Type == EStateTreeStateType::Linked)
		{
			Extra->SetStringField(Field::LinkedSubtree, DescribeStateLink(InState.LinkedSubtree, &InState));
		}
		if (InState.bHasRequiredEventToEnter)
		{
			TSharedPtr<FJsonObject> EventJson = MakeShared<FJsonObject>();
			WriteEventDesc(InState.RequiredEventToEnter, EventJson);
			Extra->SetObjectField(Field::RequiredEvent, EventJson);
		}

		TSharedPtr<FJsonObject> Parameters;
		ExportPropertyBag(
			InState.Parameters.Parameters,
			Parameters,
			OutDiagnostics,
			InFieldPath + TEXT(".parameters"));
		if (Parameters.IsValid() && Parameters->Values.Num() > 0)
		{
			Extra->SetObjectField(Field::Parameters, Parameters);
		}

		if (InState.EnterConditions.Num() > 0)
		{
			Extra->SetArrayField(
				Field::EnterConditions,
				ReadEditorNodes(InState.EnterConditions, OutDiagnostics, InFieldPath + TEXT(".enterConditions")));
		}
		if (InState.Tasks.Num() > 0)
		{
			Extra->SetArrayField(
				Field::Tasks,
				ReadEditorNodes(InState.Tasks, OutDiagnostics, InFieldPath + TEXT(".tasks")));
		}
		if (InState.Considerations.Num() > 0)
		{
			Extra->SetArrayField(
				Field::Considerations,
				ReadEditorNodes(InState.Considerations, OutDiagnostics, InFieldPath + TEXT(".considerations")));
		}
		if (InState.SingleTask.GetNode().IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>> Single = ReadEditorNodes(
				MakeArrayView(&InState.SingleTask, 1), OutDiagnostics, InFieldPath + TEXT(".singleTask"));
			if (Single.Num() > 0)
			{
				Extra->SetField(Field::SingleTask, Single[0]);
			}
		}

		if (InState.Transitions.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Transitions;
			for (int32 Index = 0; Index < InState.Transitions.Num(); ++Index)
			{
				Transitions.Add(MakeShared<FJsonValueObject>(ReadTransition(
					InState.Transitions[Index],
					InState,
					OutDiagnostics,
					FString::Printf(TEXT("%s.transitions[%d]"), *InFieldPath, Index))));
			}
			Extra->SetArrayField(Field::Transitions, Transitions);
		}

		OutNode.Extra = Extra;

		for (const TObjectPtr<UStateTreeState>& Child : InState.Children)
		{
			if (Child == nullptr)
			{
				continue;
			}

			FForgeNode ChildNode;
			ReadStateInto(
				*Child,
				ChildNode,
				OutDiagnostics,
				FString::Printf(TEXT("%s/%s"), *InFieldPath, *Child->Name.ToString()));
			OutNode.Children.Add(MoveTemp(ChildNode));
		}
	}

	// ---- 参数（PropertyBag）-----

	FString BagTypeToString(const EPropertyBagPropertyType InType)
	{
		switch (InType)
		{
		case EPropertyBagPropertyType::Bool: return TEXT("Bool");
		case EPropertyBagPropertyType::Byte: return TEXT("Byte");
		case EPropertyBagPropertyType::Int32: return TEXT("Int32");
		case EPropertyBagPropertyType::Int64: return TEXT("Int64");
		case EPropertyBagPropertyType::Float: return TEXT("Float");
		case EPropertyBagPropertyType::Double: return TEXT("Double");
		case EPropertyBagPropertyType::Name: return TEXT("Name");
		case EPropertyBagPropertyType::String: return TEXT("String");
		case EPropertyBagPropertyType::Text: return TEXT("Text");
		case EPropertyBagPropertyType::Enum: return TEXT("Enum");
		case EPropertyBagPropertyType::Struct: return TEXT("Struct");
		case EPropertyBagPropertyType::Object: return TEXT("Object");
		case EPropertyBagPropertyType::SoftObject: return TEXT("SoftObject");
		case EPropertyBagPropertyType::Class: return TEXT("Class");
		case EPropertyBagPropertyType::SoftClass: return TEXT("SoftClass");
		case EPropertyBagPropertyType::Int8: return TEXT("Int8");
		case EPropertyBagPropertyType::Int16: return TEXT("Int16");
		case EPropertyBagPropertyType::UInt16: return TEXT("UInt16");
		case EPropertyBagPropertyType::UInt32: return TEXT("UInt32");
		case EPropertyBagPropertyType::UInt64: return TEXT("UInt64");
		default: return FString();
		}
	}

	bool BagTypeFromString(const FString& InText, EPropertyBagPropertyType& OutType)
	{
		static const TMap<FString, EPropertyBagPropertyType> Table = {
			{ TEXT("Bool"), EPropertyBagPropertyType::Bool },
			{ TEXT("Byte"), EPropertyBagPropertyType::Byte },
			{ TEXT("Int32"), EPropertyBagPropertyType::Int32 },
			{ TEXT("Int64"), EPropertyBagPropertyType::Int64 },
			{ TEXT("Float"), EPropertyBagPropertyType::Float },
			{ TEXT("Double"), EPropertyBagPropertyType::Double },
			{ TEXT("Name"), EPropertyBagPropertyType::Name },
			{ TEXT("String"), EPropertyBagPropertyType::String },
			{ TEXT("Text"), EPropertyBagPropertyType::Text },
			{ TEXT("Enum"), EPropertyBagPropertyType::Enum },
			{ TEXT("Struct"), EPropertyBagPropertyType::Struct },
			{ TEXT("Object"), EPropertyBagPropertyType::Object },
			{ TEXT("SoftObject"), EPropertyBagPropertyType::SoftObject },
			{ TEXT("Class"), EPropertyBagPropertyType::Class },
			{ TEXT("SoftClass"), EPropertyBagPropertyType::SoftClass },
			{ TEXT("Int8"), EPropertyBagPropertyType::Int8 },
			{ TEXT("Int16"), EPropertyBagPropertyType::Int16 },
			{ TEXT("UInt16"), EPropertyBagPropertyType::UInt16 },
			{ TEXT("UInt32"), EPropertyBagPropertyType::UInt32 },
			{ TEXT("UInt64"), EPropertyBagPropertyType::UInt64 },
		};

		if (const EPropertyBagPropertyType* Found = Table.Find(InText))
		{
			OutType = *Found;
			return true;
		}

		return false;
	}

	/** 显式类型对象形式（`{"type": ..., "value": ...}`）的包装。 */
	bool IsExplicitBagValue(const TSharedPtr<FJsonValue>& InValue)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		return InValue.IsValid() && InValue->TryGetObject(Object) && Object->IsValid()
			&& (*Object)->HasField(TEXT("type"));
	}

	/** 从裸值推断参数类型（新参数专用；字符串一律推断为 String）。 */
	bool InferBagType(const FJsonValue& InValue, EPropertyBagPropertyType& OutType, FString& OutError)
	{
		switch (InValue.Type)
		{
		case EJson::Boolean:
			OutType = EPropertyBagPropertyType::Bool;
			return true;
		case EJson::Number:
		{
			const double Number = InValue.AsNumber();
			const bool bFitsInt32 = FMath::Abs(Number) < 2147483647.0
				&& FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number));
			OutType = bFitsInt32 ? EPropertyBagPropertyType::Int32 : EPropertyBagPropertyType::Float;
			return true;
		}
		case EJson::String:
			OutType = EPropertyBagPropertyType::String;
			return true;
		default:
			OutError = TEXT("无法从该值推断参数类型；请写显式形式 {\"type\": \"...\", \"value\": ...}");
			return false;
		}
	}

	/** 导出单个参数值；返回 false 时 OutError 说明原因（诊断由调用方生成）。 */
	bool ExportBagValue(
		const FInstancedPropertyBag& InBag,
		const FPropertyBagPropertyDesc& InDesc,
		TSharedPtr<FJsonValue>& OutValue,
		FString& OutError)
	{
		const FName Name = InDesc.Name;

		auto Fail = [&OutError, &Name]()
		{
			OutError = FString::Printf(TEXT("读取参数 `%s` 失败"), *Name.ToString());
			return false;
		};

		switch (InDesc.ValueType)
		{
		case EPropertyBagPropertyType::Bool:
			if (auto R = InBag.GetValueBool(InDesc); R.IsValid())
			{
				OutValue = MakeShared<FJsonValueBoolean>(R.GetValue());
				return true;
			}
			return Fail();
		case EPropertyBagPropertyType::Byte:
			if (auto R = InBag.GetValueByte(InDesc); R.IsValid())
			{
				OutValue = MakeShared<FJsonValueNumber>(R.GetValue());
				return true;
			}
			return Fail();
		case EPropertyBagPropertyType::Int8:
			if (auto R = InBag.GetValueInt8(InDesc); R.IsValid())
			{
				OutValue = MakeShared<FJsonValueNumber>(R.GetValue());
				return true;
			}
			return Fail();
		case EPropertyBagPropertyType::Int16:
			if (auto R = InBag.GetValueInt16(InDesc); R.IsValid())
			{
				OutValue = MakeShared<FJsonValueNumber>(R.GetValue());
				return true;
			}
			return Fail();
		case EPropertyBagPropertyType::UInt16:
			if (auto R = InBag.GetValueUInt16(InDesc); R.IsValid())
			{
				OutValue = MakeShared<FJsonValueNumber>(R.GetValue());
				return true;
			}
			return Fail();
		case EPropertyBagPropertyType::Int32:
			if (auto R = InBag.GetValueInt32(InDesc); R.IsValid())
			{
				OutValue = MakeShared<FJsonValueNumber>(R.GetValue());
				return true;
			}
			return Fail();
		case EPropertyBagPropertyType::UInt32:
			if (auto R = InBag.GetValueUInt32(InDesc); R.IsValid())
			{
				OutValue = MakeShared<FJsonValueNumber>(R.GetValue());
				return true;
			}
			return Fail();
		case EPropertyBagPropertyType::Int64:
			if (auto R = InBag.GetValueInt64(InDesc); R.IsValid())
			{
				OutValue = MakeShared<FJsonValueNumber>(static_cast<double>(R.GetValue()));
				return true;
			}
			return Fail();
		case EPropertyBagPropertyType::UInt64:
			if (auto R = InBag.GetValueUInt64(InDesc); R.IsValid())
			{
				OutValue = MakeShared<FJsonValueNumber>(static_cast<double>(R.GetValue()));
				return true;
			}
			return Fail();
		case EPropertyBagPropertyType::Float:
			if (auto R = InBag.GetValueFloat(InDesc); R.IsValid())
			{
				OutValue = ForgePropertyCodec::ShortenFloatValue(R.GetValue());
				return true;
			}
			return Fail();
		case EPropertyBagPropertyType::Double:
			if (auto R = InBag.GetValueDouble(InDesc); R.IsValid())
			{
				OutValue = MakeShared<FJsonValueNumber>(R.GetValue());
				return true;
			}
			return Fail();
		case EPropertyBagPropertyType::Name:
			if (auto R = InBag.GetValueName(InDesc); R.IsValid())
			{
				const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
				Object->SetStringField(TEXT("type"), TEXT("Name"));
				Object->SetStringField(TEXT("value"), R.GetValue().ToString());
				OutValue = MakeShared<FJsonValueObject>(Object);
				return true;
			}
			return Fail();
		case EPropertyBagPropertyType::String:
			if (auto R = InBag.GetValueString(InDesc); R.IsValid())
			{
				OutValue = MakeShared<FJsonValueString>(R.GetValue());
				return true;
			}
			return Fail();
		case EPropertyBagPropertyType::Text:
			if (auto R = InBag.GetValueText(InDesc); R.IsValid())
			{
				const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
				Object->SetStringField(TEXT("type"), TEXT("Text"));
				Object->SetStringField(TEXT("value"), R.GetValue().ToString());
				OutValue = MakeShared<FJsonValueObject>(Object);
				return true;
			}
			return Fail();
		case EPropertyBagPropertyType::Enum:
		{
			const UEnum* EnumType = Cast<const UEnum>(InDesc.ValueTypeObject);
			if (EnumType == nullptr)
			{
				OutError = TEXT("枚举参数缺少枚举类型");
				return false;
			}
			if (auto R = InBag.GetValueEnumInt64(InDesc, EnumType); R.IsValid())
			{
				const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
				Object->SetStringField(TEXT("type"), TEXT("Enum"));
				Object->SetStringField(TEXT("enum"), EnumType->GetPathName());
				Object->SetStringField(TEXT("value"), EnumType->GetNameStringByValue(R.GetValue()));
				OutValue = MakeShared<FJsonValueObject>(Object);
				return true;
			}
			return Fail();
		}
		case EPropertyBagPropertyType::Struct:
		{
			const UScriptStruct* StructType = Cast<const UScriptStruct>(InDesc.ValueTypeObject);
			if (StructType == nullptr)
			{
				OutError = TEXT("结构体参数缺少结构体类型");
				return false;
			}
			if (auto R = InBag.GetValueStruct(InDesc, StructType); R.IsValid() && R.GetValue().IsValid())
			{
				TSharedPtr<FJsonObject> StructJson = MakeShared<FJsonObject>();
				FJsonObjectConverter::UStructToJsonObject(StructType, R.GetValue().GetMemory(), StructJson.ToSharedRef(), 0, 0);
				const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
				Object->SetStringField(TEXT("type"), TEXT("Struct"));
				Object->SetStringField(TEXT("struct"), StructType->GetPathName());
				Object->SetObjectField(TEXT("value"), StructJson);
				OutValue = MakeShared<FJsonValueObject>(Object);
				return true;
			}
			return Fail();
		}
		case EPropertyBagPropertyType::Object:
		case EPropertyBagPropertyType::SoftObject:
		case EPropertyBagPropertyType::Class:
		case EPropertyBagPropertyType::SoftClass:
		{
			const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("type"), BagTypeToString(InDesc.ValueType));
			if (const UClass* ConstraintClass = Cast<const UClass>(InDesc.ValueTypeObject))
			{
				Object->SetStringField(TEXT("class"), ConstraintClass->GetPathName());
			}

			FString ValuePath;
			if (InDesc.ValueType == EPropertyBagPropertyType::Object)
			{
				if (auto R = InBag.GetValueObject(InDesc); R.IsValid())
				{
					ValuePath = R.GetValue() ? R.GetValue()->GetPathName() : FString();
				}
			}
			else if (InDesc.ValueType == EPropertyBagPropertyType::Class)
			{
				if (auto R = InBag.GetValueClass(InDesc); R.IsValid())
				{
					ValuePath = R.GetValue() ? R.GetValue()->GetPathName() : FString();
				}
			}
			else
			{
				if (auto R = InBag.GetValueSoftPath(InDesc); R.IsValid())
				{
					ValuePath = R.GetValue().ToString();
				}
			}

			Object->SetStringField(TEXT("value"), ValuePath);
			OutValue = MakeShared<FJsonValueObject>(Object);
			return true;
		}
		default:
			OutError = FString::Printf(
				TEXT("参数类型 `%s` 暂不支持（本版本支持标量、枚举、结构体与对象引用）"),
				*BagTypeToString(InDesc.ValueType));
			return false;
		}
	}

	/** 写单个参数值；返回 false 时 OutError 说明原因。 */
	bool ImportBagValue(
		FInstancedPropertyBag& InOutBag,
		const FPropertyBagPropertyDesc& InDesc,
		const TSharedPtr<FJsonValue>& InValue,
		FString& OutError)
	{
		const FName Name = InDesc.Name;

		// 显式形式解包：写值只关心 `value` 字段（类型由 desc 决定）。
		TSharedPtr<FJsonValue> Value = InValue;
		if (IsExplicitBagValue(InValue))
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (InValue->TryGetObject(Object))
			{
				Value = (*Object)->TryGetField(TEXT("value"));
			}
			if (!Value.IsValid())
			{
				Value = MakeShared<FJsonValueNull>();
			}
		}

		auto Ok = [](const EPropertyBagResult InResult)
		{
			return InResult == EPropertyBagResult::Success;
		};
		auto Fail = [&OutError, &Name](const EPropertyBagResult InResult)
		{
			OutError = FString::Printf(
				TEXT("写参数 `%s` 失败（%s）"),
				*Name.ToString(),
				*StaticEnum<EPropertyBagResult>()->GetNameStringByValue(static_cast<int64>(InResult)));
			return false;
		};

		switch (InDesc.ValueType)
		{
		case EPropertyBagPropertyType::Bool:
			return Ok(InOutBag.SetValueBool(Name, Value->AsBool())) ? true : Fail(InOutBag.SetValueBool(Name, Value->AsBool()));
		case EPropertyBagPropertyType::Byte:
		{
			const uint8 Converted = static_cast<uint8>(Value->AsNumber());
			return Ok(InOutBag.SetValueByte(Name, Converted)) ? true : Fail(InOutBag.SetValueByte(Name, Converted));
		}
		case EPropertyBagPropertyType::Int8:
		{
			const int8 Converted = static_cast<int8>(Value->AsNumber());
			return Ok(InOutBag.SetValueInt8(Name, Converted)) ? true : Fail(InOutBag.SetValueInt8(Name, Converted));
		}
		case EPropertyBagPropertyType::Int16:
		{
			const int16 Converted = static_cast<int16>(Value->AsNumber());
			return Ok(InOutBag.SetValueInt16(Name, Converted)) ? true : Fail(InOutBag.SetValueInt16(Name, Converted));
		}
		case EPropertyBagPropertyType::UInt16:
		{
			const uint16 Converted = static_cast<uint16>(Value->AsNumber());
			return Ok(InOutBag.SetValueUInt16(Name, Converted)) ? true : Fail(InOutBag.SetValueUInt16(Name, Converted));
		}
		case EPropertyBagPropertyType::Int32:
		{
			const int32 Converted = static_cast<int32>(Value->AsNumber());
			return Ok(InOutBag.SetValueInt32(Name, Converted)) ? true : Fail(InOutBag.SetValueInt32(Name, Converted));
		}
		case EPropertyBagPropertyType::UInt32:
		{
			const uint32 Converted = static_cast<uint32>(Value->AsNumber());
			return Ok(InOutBag.SetValueUInt32(Name, Converted)) ? true : Fail(InOutBag.SetValueUInt32(Name, Converted));
		}
		case EPropertyBagPropertyType::Int64:
		{
			const int64 Converted = static_cast<int64>(Value->AsNumber());
			return Ok(InOutBag.SetValueInt64(Name, Converted)) ? true : Fail(InOutBag.SetValueInt64(Name, Converted));
		}
		case EPropertyBagPropertyType::UInt64:
		{
			const uint64 Converted = static_cast<uint64>(Value->AsNumber());
			return Ok(InOutBag.SetValueUInt64(Name, Converted)) ? true : Fail(InOutBag.SetValueUInt64(Name, Converted));
		}
		case EPropertyBagPropertyType::Float:
		{
			const float Converted = static_cast<float>(Value->AsNumber());
			return Ok(InOutBag.SetValueFloat(Name, Converted)) ? true : Fail(InOutBag.SetValueFloat(Name, Converted));
		}
		case EPropertyBagPropertyType::Double:
		{
			const double Converted = Value->AsNumber();
			return Ok(InOutBag.SetValueDouble(Name, Converted)) ? true : Fail(InOutBag.SetValueDouble(Name, Converted));
		}
		case EPropertyBagPropertyType::Name:
		{
			const FName Converted(*Value->AsString());
			return Ok(InOutBag.SetValueName(Name, Converted)) ? true : Fail(InOutBag.SetValueName(Name, Converted));
		}
		case EPropertyBagPropertyType::String:
		{
			const FString Converted = Value->AsString();
			return Ok(InOutBag.SetValueString(Name, Converted)) ? true : Fail(InOutBag.SetValueString(Name, Converted));
		}
		case EPropertyBagPropertyType::Text:
		{
			const FText Converted = FText::FromString(Value->AsString());
			return Ok(InOutBag.SetValueText(Name, Converted)) ? true : Fail(InOutBag.SetValueText(Name, Converted));
		}
		case EPropertyBagPropertyType::Enum:
		{
			const UEnum* EnumType = Cast<const UEnum>(InDesc.ValueTypeObject);
			if (EnumType == nullptr)
			{
				OutError = TEXT("枚举参数缺少枚举类型");
				return false;
			}
			int64 EnumValue = EnumType->GetValueByNameString(Value->AsString());
			if (EnumValue == INDEX_NONE && Value->Type == EJson::Number)
			{
				EnumValue = static_cast<int64>(Value->AsNumber());
			}
			if (EnumValue == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("枚举 `%s` 没有取值 `%s`"), *EnumType->GetName(), *Value->AsString());
				return false;
			}
			return Ok(InOutBag.SetValueEnum(Name, EnumValue, EnumType)) ? true : Fail(InOutBag.SetValueEnum(Name, EnumValue, EnumType));
		}
		case EPropertyBagPropertyType::Struct:
		{
			const UScriptStruct* StructType = Cast<const UScriptStruct>(InDesc.ValueTypeObject);
			if (StructType == nullptr)
			{
				OutError = TEXT("结构体参数缺少结构体类型");
				return false;
			}
			const TSharedPtr<FJsonObject>* StructJson = nullptr;
			if (!Value->TryGetObject(StructJson) || !StructJson->IsValid())
			{
				OutError = TEXT("结构体参数的 value 必须是 JSON 对象");
				return false;
			}
			FInstancedStruct Converted;
			Converted.InitializeAs(StructType);
			if (!FJsonObjectConverter::JsonObjectToUStruct((*StructJson).ToSharedRef(), StructType, Converted.GetMutableMemory(), 0, 0))
			{
				OutError = TEXT("结构体值的字段与结构体定义不匹配");
				return false;
			}
			return Ok(InOutBag.SetValueStruct(Name, FConstStructView(StructType, Converted.GetMemory())))
				? true
				: Fail(InOutBag.SetValueStruct(Name, FConstStructView(StructType, Converted.GetMemory())));
		}
		case EPropertyBagPropertyType::Object:
		{
			UObject* Converted = nullptr;
			if (!Value->AsString().IsEmpty())
			{
				Converted = LoadObject<UObject>(nullptr, *Value->AsString());
				if (Converted == nullptr)
				{
					OutError = FString::Printf(TEXT("找不到对象 `%s`"), *Value->AsString());
					return false;
				}
			}
			return Ok(InOutBag.SetValueObject(Name, Converted)) ? true : Fail(InOutBag.SetValueObject(Name, Converted));
		}
		case EPropertyBagPropertyType::Class:
		{
			UClass* Converted = nullptr;
			if (!Value->AsString().IsEmpty())
			{
				Converted = LoadClass<UObject>(nullptr, *Value->AsString());
				if (Converted == nullptr)
				{
					OutError = FString::Printf(TEXT("找不到类 `%s`"), *Value->AsString());
					return false;
				}
			}
			return Ok(InOutBag.SetValueClass(Name, Converted)) ? true : Fail(InOutBag.SetValueClass(Name, Converted));
		}
		case EPropertyBagPropertyType::SoftObject:
		case EPropertyBagPropertyType::SoftClass:
		{
			const FSoftObjectPath Converted(Value->AsString());
			return Ok(InOutBag.SetValueSoftPath(Name, Converted)) ? true : Fail(InOutBag.SetValueSoftPath(Name, Converted));
		}
		default:
			OutError = FString::Printf(TEXT("参数类型 `%s` 暂不支持"), *BagTypeToString(InDesc.ValueType));
			return false;
		}
	}

	/** 解析显式类型里的 `enum` / `struct` / `class` 字段。 */
	const UObject* ResolveTypeObject(
		const TSharedPtr<FJsonObject>& InExplicit,
		const EPropertyBagPropertyType InType,
		FString& OutError)
	{
		FString Path;
		switch (InType)
		{
		case EPropertyBagPropertyType::Enum:
			InExplicit->TryGetStringField(TEXT("enum"), Path);
			if (Path.IsEmpty())
			{
				OutError = TEXT("Enum 参数需要 `enum` 字段（枚举类路径）");
				return nullptr;
			}
			if (UEnum* Enum = LoadObject<UEnum>(nullptr, *Path))
			{
				return Enum;
			}
			OutError = FString::Printf(TEXT("找不到枚举 `%s`"), *Path);
			return nullptr;
		case EPropertyBagPropertyType::Struct:
			InExplicit->TryGetStringField(TEXT("struct"), Path);
			if (Path.IsEmpty())
			{
				OutError = TEXT("Struct 参数需要 `struct` 字段（结构体路径）");
				return nullptr;
			}
			if (UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *Path))
			{
				return Struct;
			}
			OutError = FString::Printf(TEXT("找不到结构体 `%s`"), *Path);
			return nullptr;
		case EPropertyBagPropertyType::Object:
		case EPropertyBagPropertyType::SoftObject:
		case EPropertyBagPropertyType::Class:
		case EPropertyBagPropertyType::SoftClass:
			// 约束类可空（不写就是不限）。
			InExplicit->TryGetStringField(TEXT("class"), Path);
			if (Path.IsEmpty())
			{
				return nullptr;
			}
			if (UClass* Class = LoadClass<UObject>(nullptr, *Path))
			{
				return Class;
			}
			OutError = FString::Printf(TEXT("找不到类 `%s`"), *Path);
			return nullptr;
		default:
			return nullptr;
		}
	}

} // namespace

bool IsNodeTypeMatch(const FStateTreeEditorNode& InNode, UScriptStruct* InStruct, UClass* InClass)
{
	const UScriptStruct* NodeStruct = InNode.GetNode().GetScriptStruct();
	if (NodeStruct == nullptr)
	{
		return false;
	}

	if (InStruct != nullptr)
	{
		return NodeStruct == InStruct;
	}

	if (InClass != nullptr)
	{
		return GetBlueprintNodeClass(InNode) == InClass;
	}

	return false;
}

bool ParseEnumByName(const UEnum* InEnum, const FString& InText, int64& OutValue, FString& OutError)
{
	if (InEnum == nullptr)
	{
		OutError = TEXT("枚举类型不可用");
		return false;
	}

	int64 Value = InEnum->GetValueByNameString(InText);
	if (Value == INDEX_NONE && InText.Contains(TEXT("::")))
	{
		Value = InEnum->GetValueByNameString(InText.RightChop(InText.Find(TEXT("::")) + 2));
	}

	if (Value == INDEX_NONE)
	{
		OutError = FString::Printf(
			TEXT("`%s` 不是 %s 的合法取值"),
			*InText,
			*InEnum->GetName());
		return false;
	}

	OutValue = Value;
	return true;
}

void ExportDocument(
	const UStateTree& InTree,
	FForgeDocument& OutDocument,
	TArray<FForgeDiagnostic>& OutDiagnostics)
{
	const UStateTreeEditorData* EditorData = Cast<const UStateTreeEditorData>(InTree.EditorData.Get());
	if (EditorData == nullptr)
	{
		OutDiagnostics.Emplace(
			EForgeSeverity::Error,
			TEXT("asset"),
			FString::Printf(TEXT("%s 没有编辑器数据（EditorData），无法导出"), *InTree.GetPathName()));
		return;
	}

	OutDocument.Kind = TEXT("state-tree");
	OutDocument.SchemaVersion = ForgeDoc::CurrentSchemaVersion;
	// 写包路径（不是对象路径），与其它 kind 的导出写法一致。
	OutDocument.Asset = InTree.GetOutermost()->GetName();

	const TSharedPtr<FJsonObject> Extra = MakeShared<FJsonObject>();

	if (EditorData->Schema != nullptr)
	{
		// 写类路径（不是对象路径）：schema 是资产内的实例，配置要的是「用哪个类重建」。
		Extra->SetStringField(Field::Schema, EditorData->Schema->GetClass()->GetPathName());
	}
	else
	{
		OutDiagnostics.Emplace(
			EForgeSeverity::Warning,
			TEXT("schema"),
			TEXT("资产没有 Schema；导入时必须能解析到 Schema 才能重建"));
	}

	TSharedPtr<FJsonObject> RootParameters;
	ExportPropertyBag(EditorData->GetRootParametersPropertyBag(), RootParameters, OutDiagnostics, TEXT("parameters"));
	if (RootParameters.IsValid() && RootParameters->Values.Num() > 0)
	{
		Extra->SetObjectField(Field::Parameters, RootParameters);
	}

	if (EditorData->Evaluators.Num() > 0)
	{
		Extra->SetArrayField(Field::Evaluators, ReadEditorNodes(EditorData->Evaluators, OutDiagnostics, TEXT("evaluators")));
	}
	if (EditorData->GlobalTasks.Num() > 0)
	{
		Extra->SetArrayField(Field::GlobalTasks, ReadEditorNodes(EditorData->GlobalTasks, OutDiagnostics, TEXT("globalTasks")));
	}
	if (EditorData->GlobalTasksCompletion != EStateTreeTaskCompletionType::Any)
	{
		Extra->SetStringField(Field::GlobalTasksCompletion, EnumToString(EditorData->GlobalTasksCompletion));
	}

	// 绑定：全量写出，按应用后的字符串排序（两次导出字节稳定是收敛判据的一部分）。
	{
		TMap<FGuid, FString> References;
		CollectStructReferences(*EditorData, References);

		TArray<TPair<FString, FString>> Entries;
		EditorData->GetPropertyEditorBindings()->ForEachBinding(
			[&References, &Entries, &OutDiagnostics](const FPropertyBindingBinding& Binding)
			{
				// 路径文本（配置用）：段一律用属性名（`GetName`），不用编辑器显示名（`GetDisplayName`）。
				// 引擎的 `ToString` 优先输出显示名（`bLeft` → `Left`、`LinkFlag` → `Link Flag`），
				// 与配置约定（属性名）不一致，会让「导出 → 再导入」的收敛判据抖动。
				auto MakeConfigPathText = [](const FPropertyBindingPath& InPath) -> FString
				{
					FString Text;
					const TConstArrayView<FPropertyBindingPathSegment> Segments = InPath.GetSegments();
					for (int32 Index = 0; Index < Segments.Num(); ++Index)
					{
						if (Index > 0)
						{
							Text.AppendChar(TEXT('.'));
						}
						Text += Segments[Index].GetName().ToString();
						if (Segments[Index].GetArrayIndex() >= 0)
						{
							Text += FString::Printf(TEXT("[%d]"), Segments[Index].GetArrayIndex());
						}
					}
					return Text;
				};

				auto MakeReference = [&References, &OutDiagnostics, &MakeConfigPathText](const FPropertyBindingPath& InPath) -> FString
				{
					const FString* Base = References.Find(InPath.GetStructID());
					if (Base == nullptr)
					{
						OutDiagnostics.Emplace(
							EForgeSeverity::Warning,
							TEXT("bindings"),
							FString::Printf(
								TEXT("绑定指向了不可识别、不可导出的结构（ID %s），该条被跳过"),
								*InPath.GetStructID().ToString()));
						return FString();
					}

					if (InPath.IsPathEmpty())
					{
						return *Base;
					}

					return FString::Printf(TEXT("%s.%s"), **Base, *MakeConfigPathText(InPath));
				};

				const FString Source = MakeReference(Binding.GetSourcePath());
				const FString Target = MakeReference(Binding.GetTargetPath());
				if (!Source.IsEmpty() && !Target.IsEmpty())
				{
					Entries.Emplace(Source, Target);
				}
			});

		Entries.Sort([](const TPair<FString, FString>& A, const TPair<FString, FString>& B)
		{
			return A.Value == B.Value ? A.Key < B.Key : A.Value < B.Value;
		});

		if (Entries.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Bindings;
			for (const TPair<FString, FString>& Entry : Entries)
			{
				const TSharedPtr<FJsonObject> BindingJson = MakeShared<FJsonObject>();
				BindingJson->SetStringField(Field::Source, Entry.Key);
				BindingJson->SetStringField(Field::Target, Entry.Value);
				Bindings.Add(MakeShared<FJsonValueObject>(BindingJson));
			}
			Extra->SetArrayField(Field::Bindings, Bindings);
		}
	}

	OutDocument.Extra = Extra;

	// 根状态：本版本单根，`SubTrees` 多根时给诊断（其余子树不导出）。
	if (EditorData->SubTrees.Num() == 0 || EditorData->SubTrees[0] == nullptr)
	{
		OutDiagnostics.Emplace(EForgeSeverity::Error, TEXT("root"), TEXT("资产没有根状态"));
		return;
	}
	if (EditorData->SubTrees.Num() > 1)
	{
		OutDiagnostics.Emplace(
			EForgeSeverity::Warning,
			TEXT("root"),
			FString::Printf(
				TEXT("资产有 %d 个顶层子树，本版本只导出第一个；多子树是已知限制"),
				EditorData->SubTrees.Num()));
	}

	ReadStateInto(*EditorData->SubTrees[0], OutDocument.Root, OutDiagnostics, TEXT("root"));
}

void CollectStructReferences(const UStateTreeEditorData& InEditorData, TMap<FGuid, FString>& OutReferences)
{
	OutReferences.Reset();

	InEditorData.VisitAllNodes(
		[&OutReferences](const UStateTreeState* State, const FStateTreeBindableStructDesc& Desc, const FStateTreeDataView Value)
		{
			const FString Name = Desc.Name.ToString();
			const FString Reference = Desc.StatePath.IsEmpty()
				? Name
				: FString::Printf(TEXT("%s/%s"), *Desc.StatePath, *Name);
			OutReferences.Add(Desc.ID, Reference);
			return EStateTreeVisitor::Continue;
		});
}

namespace
{
	/** 递归收集节点模板条目的 ID；解析引用时要跳过它们。 */
	void CollectNodeTemplateIDs(const UStateTreeState& InState, TSet<FGuid>& OutTemplateIDs)
	{
		auto CollectNodes = [&OutTemplateIDs](const TArray<FStateTreeEditorNode>& InNodes)
		{
			for (const FStateTreeEditorNode& Node : InNodes)
			{
				if (Node.GetNode().IsValid())
				{
					OutTemplateIDs.Add(Node.GetNodeID());
				}
			}
		};

		CollectNodes(InState.EnterConditions);
		CollectNodes(InState.Tasks);
		CollectNodes(InState.Considerations);
		if (InState.SingleTask.GetNode().IsValid())
		{
			OutTemplateIDs.Add(InState.SingleTask.GetNodeID());
		}
		for (const FStateTreeTransition& Transition : InState.Transitions)
		{
			CollectNodes(Transition.Conditions);
		}
		for (const TObjectPtr<UStateTreeState>& Child : InState.Children)
		{
			if (Child != nullptr)
			{
				CollectNodeTemplateIDs(*Child, OutTemplateIDs);
			}
		}
	}

	void CollectNodeTemplateIDs(const UStateTreeEditorData& InEditorData, TSet<FGuid>& OutTemplateIDs)
	{
		auto CollectNodes = [&OutTemplateIDs](const TArray<FStateTreeEditorNode>& InNodes)
		{
			for (const FStateTreeEditorNode& Node : InNodes)
			{
				if (Node.GetNode().IsValid())
				{
					OutTemplateIDs.Add(Node.GetNodeID());
				}
			}
		};

		CollectNodes(InEditorData.Evaluators);
		CollectNodes(InEditorData.GlobalTasks);
		for (const TObjectPtr<UStateTreeState>& Root : InEditorData.SubTrees)
		{
			if (Root != nullptr)
			{
				CollectNodeTemplateIDs(*Root, OutTemplateIDs);
			}
		}
	}
} // namespace

bool ResolveStructReference(
	const UStateTreeEditorData& InEditorData,
	const FString& InReference,
	FGuid& OutStructID,
	FString& OutPropertyPath,
	FString& OutError)
{
	// 任务等节点在枚举里有两套条目（实例数据与节点模板，同名同路径）；引用一律按实例数据解析，
	// 否则每个命名节点都会"自撞"成歧义。
	TSet<FGuid> TemplateIDs;
	CollectNodeTemplateIDs(InEditorData, TemplateIDs);

	TArray<TPair<FString, FGuid>> Candidates;
	InEditorData.VisitAllNodes(
		[&Candidates, &TemplateIDs](const UStateTreeState* State, const FStateTreeBindableStructDesc& Desc, const FStateTreeDataView Value)
		{
			if (TemplateIDs.Contains(Desc.ID))
			{
				return EStateTreeVisitor::Continue;
			}

			const FString Name = Desc.Name.ToString();
			const FString Reference = Desc.StatePath.IsEmpty()
				? Name
				: FString::Printf(TEXT("%s/%s"), *Desc.StatePath, *Name);
			Candidates.Emplace(Reference, Desc.ID);
			return EStateTreeVisitor::Continue;
		});

	// 最长前缀匹配：`Root/Combat/Wait` 与 `Root/Combat/Wait2` 同时存在时取恰好是前缀的那个；
	// 同长度的多个候选（同名节点等）判为歧义。
	int32 BestLength = INDEX_NONE;
	int32 MatchCount = 0;
	FString BestReference;

	for (const TPair<FString, FGuid>& Candidate : Candidates)
	{
		const FString& Reference = Candidate.Key;
		const bool bExact = InReference == Reference;
		const bool bPrefix = InReference.Len() > Reference.Len()
			&& InReference.StartsWith(Reference, ESearchCase::CaseSensitive)
			&& InReference[Reference.Len()] == TEXT('.');

		if (!bExact && !bPrefix)
		{
			continue;
		}

		if (Reference.Len() > BestLength)
		{
			BestLength = Reference.Len();
			MatchCount = 1;
			BestReference = Reference;
			OutStructID = Candidate.Value;
		}
		else if (Reference.Len() == BestLength)
		{
			++MatchCount;
		}
	}

	if (BestLength == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("引用 `%s` 匹配不到任何可绑定结构（状态参数 / 节点 / 全局项）"), *InReference);
		return false;
	}

	if (MatchCount > 1)
	{
		OutError = FString::Printf(
			TEXT("引用 `%s` 存在歧义：多个可绑定结构对应 `%s`；给节点起唯一的 name 可消除歧义"),
			*InReference,
			*BestReference);
		return false;
	}

	OutPropertyPath = InReference.Mid(BestLength);
	OutPropertyPath.RemoveFromStart(TEXT("."));
	return true;
}

bool ResolveNodeType(
	const FString& InType,
	UScriptStruct*& OutStruct,
	UClass*& OutClass,
	FString& OutError)
{
	OutStruct = nullptr;
	OutClass = nullptr;

	if (InType.IsEmpty())
	{
		OutError = TEXT("节点缺少 type");
		return false;
	}

	if (UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *InType))
	{
		if (!Struct->IsChildOf(TBaseStructure<FStateTreeNodeBase>::Get()))
		{
			OutError = FString::Printf(TEXT("`%s` 不是状态树节点结构（FStateTreeNodeBase 派生）"), *InType);
			return false;
		}

		OutStruct = Struct;
		return true;
	}

	if (UClass* Class = LoadObject<UClass>(nullptr, *InType))
	{
		OutClass = Class;
		return true;
	}

	OutError = FString::Printf(
		TEXT("找不到节点类型 `%s`；C++ 节点写结构体全路径（/Script/<模块>.<结构体>），蓝图节点写类全路径（含 _C）"),
		*InType);
	return false;
}

void ExportPropertyBag(
	const FInstancedPropertyBag& InBag,
	TSharedPtr<FJsonObject>& OutJson,
	TArray<FForgeDiagnostic>& OutDiagnostics,
	const FString& InFieldPath)
{
	const UPropertyBag* BagStruct = InBag.GetPropertyBagStruct();
	if (BagStruct == nullptr)
	{
		return;
	}

	OutJson = MakeShared<FJsonObject>();
	for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
	{
		const FString FieldPath = FString::Printf(TEXT("%s.%s"), *InFieldPath, *Desc.Name.ToString());

		if (Desc.ContainerTypes.GetFirstContainerType() != EPropertyBagContainerType::None)
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Warning,
				FieldPath,
				FString::Printf(
					TEXT("参数 `%s` 是容器类型（数组 / 集合 / 映射），本版本暂不导出"),
					*Desc.Name.ToString()));
			continue;
		}

		TSharedPtr<FJsonValue> Value;
		FString Error;
		if (ExportBagValue(InBag, Desc, Value, Error))
		{
			OutJson->SetField(Desc.Name.ToString(), Value);
		}
		else
		{
			OutDiagnostics.Emplace(EForgeSeverity::Warning, FieldPath, Error);
		}
	}
}

void ImportPropertyBag(
	FInstancedPropertyBag& InOutBag,
	const TSharedPtr<FJsonObject>& InJson,
	TArray<FForgeDiagnostic>& OutDiagnostics,
	const FString& InFieldPath)
{
	// 配置未写的参数删除：配置是唯一真源（`InJson` 无效表示"配置没写"，等价于清空）。
	if (const UPropertyBag* BagStruct = InOutBag.GetPropertyBagStruct())
	{
		TArray<FName> Remove;
		for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
		{
			if (!InJson.IsValid() || !InJson->HasField(Desc.Name.ToString()))
			{
				Remove.Add(Desc.Name);
			}
		}
		for (const FName& Name : Remove)
		{
			InOutBag.RemovePropertyByName(Name);
		}
	}

	if (!InJson.IsValid())
	{
		return;
	}
	for (const auto& Pair : InJson->Values)
	{
		const FString Key(ASSETFORGE_JSON_KEY(Pair.Key));
		const FName PropertyName(*Key);
		const FString FieldPath = FString::Printf(TEXT("%s.%s"), *InFieldPath, *Key);
		const TSharedPtr<FJsonValue>& JsonValue = Pair.Value;

		// 目标类型：显式 `type` 优先；否则已有参数沿用现有类型，新参数按值推断。
		EPropertyBagPropertyType DesiredType = EPropertyBagPropertyType::None;
		const UObject* DesiredTypeObject = nullptr;
		bool bExplicit = false;

		const TSharedPtr<FJsonObject>* ExplicitObject = nullptr;
		if (IsExplicitBagValue(JsonValue) && JsonValue->TryGetObject(ExplicitObject))
		{
			bExplicit = true;

			FString TypeName;
			if (!(*ExplicitObject)->TryGetStringField(TEXT("type"), TypeName)
				|| !BagTypeFromString(TypeName, DesiredType))
			{
				OutDiagnostics.Emplace(
					EForgeSeverity::Error,
					FieldPath,
					FString::Printf(TEXT("无法识别的参数类型 `%s`"), *TypeName));
				continue;
			}

			FString TypeError;
			DesiredTypeObject = ResolveTypeObject(*ExplicitObject, DesiredType, TypeError);
			if (!TypeError.IsEmpty())
			{
				OutDiagnostics.Emplace(EForgeSeverity::Error, FieldPath, TypeError);
				continue;
			}
		}

		const FPropertyBagPropertyDesc* Existing = InOutBag.FindPropertyDescByName(PropertyName);

		if (!bExplicit)
		{
			if (Existing != nullptr)
			{
				DesiredType = Existing->ValueType;
				DesiredTypeObject = Existing->ValueTypeObject;
			}
			else
			{
				FString InferError;
				if (!InferBagType(*JsonValue, DesiredType, InferError))
				{
					OutDiagnostics.Emplace(EForgeSeverity::Error, FieldPath, InferError);
					continue;
				}
			}
		}

		// 类型不一致 → 移除重建（改类型 = 换成新参数；绑定按名重解析，不受影响）。
		if (Existing != nullptr
			&& (Existing->ValueType != DesiredType || Existing->ValueTypeObject != DesiredTypeObject))
		{
			InOutBag.RemovePropertyByName(PropertyName);
			Existing = nullptr;
		}

		if (Existing == nullptr)
		{
			const EPropertyBagAlterationResult AddResult = InOutBag.AddProperty(PropertyName, DesiredType, DesiredTypeObject);
			if (AddResult != EPropertyBagAlterationResult::Success)
			{
				OutDiagnostics.Emplace(
					EForgeSeverity::Error,
					FieldPath,
					TEXT("创建参数失败（类型与名称是否合法？）"));
				continue;
			}
			Existing = InOutBag.FindPropertyDescByName(PropertyName);
			if (Existing == nullptr)
			{
				continue;
			}
		}

		FString WriteError;
		if (!ImportBagValue(InOutBag, *Existing, JsonValue, WriteError))
		{
			OutDiagnostics.Emplace(EForgeSeverity::Error, FieldPath, WriteError);
		}
	}
}

} // namespace StateTreeCodec

#endif // ASSETFORGE_WITH_STATETREE
