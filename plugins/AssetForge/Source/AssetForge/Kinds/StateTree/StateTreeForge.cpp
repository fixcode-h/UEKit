// Copyright Epic Games, Inc. All Rights Reserved.

// 状态树只在 UE 5.8 上编（ASSETFORGE_WITH_STATETREE，见 AssetForge.Build.cs）：它依赖 5.6 才有的 PropertyBindingUtils，
// 5.4 的状态树绑定接口差别太大，这一类资产在 5.4 上先不支持。
#if ASSETFORGE_WITH_STATETREE

#include "Kinds/StateTree/StateTreeForge.h"

#include "Kinds/StateTree/StateTreeCodec.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/ForgeDiff.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgePropertyCodec.h"

#include "Blueprint/StateTreeConditionBlueprintBase.h"
#include "Blueprint/StateTreeConsiderationBlueprintBase.h"
#include "Blueprint/StateTreeEvaluatorBlueprintBase.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "GameplayTagContainer.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "StateTree.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorModule.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEditorPropertyBindings.h"
#include "StateTreeEditorSchema.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreePropertyBindings.h"
#include "StateTreeState.h"
#include "StateTreeTasksStatus.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace StateTreeForge
{

namespace
{
	namespace Field = StateTreeCodec::Field;

	// ---- 基础 ----

	UStateTree* FindStateTree(const FString& InAssetPath)
	{
		if (InAssetPath.IsEmpty())
		{
			return nullptr;
		}

		return Cast<UStateTree>(LoadObject<UObject>(nullptr, *InAssetPath));
	}

	bool SaveAssetPackage(UObject* InAsset, FString& OutError)
	{
		UPackage* Package = InAsset->GetOutermost();
		Package->MarkPackageDirty();

		const FString PackageFileName = FPaths::ConvertRelativePathToFull(
			FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()));

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Package, InAsset, *PackageFileName, SaveArgs))
		{
			OutError = FString::Printf(TEXT("保存资产失败：%s"), *PackageFileName);
			return false;
		}

		return true;
	}

	/** 配置里的名字规则：非空、不含层级分隔符 `/`（它在引用里是路径分隔）。 */
	bool ValidateConfigName(const FString& InName, const FString& InFieldPath, FForgeReport& OutReport)
	{
		if (InName.IsEmpty())
		{
			OutReport.AddError(InFieldPath, TEXT("名字不能为空"));
			return false;
		}
		if (InName.Contains(TEXT("/")))
		{
			OutReport.AddError(InFieldPath, TEXT("名字不能包含 `/`（引用里的层级分隔符）"));
			return false;
		}
		return true;
	}

	/** 读 Extra 里的枚举字段；缺失用默认，取值错误给诊断。 */
	template <typename TEnum>
	void ReadEnumField(
		const TSharedPtr<FJsonObject>& InExtra,
		const TCHAR* InField,
		TEnum InDefault,
		TEnum& OutValue,
		FForgeReport& OutReport,
		const FString& InFieldPath)
	{
		OutValue = InDefault;

		FString Text;
		if (!InExtra.IsValid() || !InExtra->TryGetStringField(InField, Text) || Text.IsEmpty())
		{
			return;
		}

		int64 Value = 0;
		FString Error;
		if (!StateTreeCodec::ParseEnumByName(StaticEnum<TEnum>(), Text, Value, Error))
		{
			OutReport.AddError(FString::Printf(TEXT("%s.%s"), *InFieldPath, InField), Error);
			return;
		}

		OutValue = static_cast<TEnum>(Value);
	}

	void ReadNumberField(
		const TSharedPtr<FJsonObject>& InExtra,
		const TCHAR* InField,
		double InDefault,
		double& OutValue,
		FForgeReport& OutReport,
		const FString& InFieldPath)
	{
		OutValue = InDefault;

		if (!InExtra.IsValid() || !InExtra->HasField(InField))
		{
			return;
		}

		if (!InExtra->TryGetNumberField(InField, OutValue))
		{
			OutReport.AddError(FString::Printf(TEXT("%s.%s"), *InFieldPath, InField), TEXT("应为数字"));
		}
	}

	void ReadBoolField(
		const TSharedPtr<FJsonObject>& InExtra,
		const TCHAR* InField,
		bool InDefault,
		bool& OutValue,
		FForgeReport& OutReport,
		const FString& InFieldPath)
	{
		OutValue = InDefault;

		if (!InExtra.IsValid() || !InExtra->HasField(InField))
		{
			return;
		}

		if (!InExtra->TryGetBoolField(InField, OutValue))
		{
			OutReport.AddError(FString::Printf(TEXT("%s.%s"), *InFieldPath, InField), TEXT("应为布尔值"));
		}
	}

	/** 把 gameplay tag 文本写进目标；空文本清空。 */
	void WriteGameplayTag(const FString& InText, FGameplayTag& OutTag)
	{
		OutTag = InText.IsEmpty() ? FGameplayTag() : FGameplayTag::RequestGameplayTag(FName(*InText), false);
	}

	const TArray<TSharedPtr<FJsonValue>>& EmptyNodeArray()
	{
		static const TArray<TSharedPtr<FJsonValue>> Empty;
		return Empty;
	}

	// ---- 节点 ----

	/** 节点列表的类别约束（C++ 结构基类与蓝图基类）。 */
	struct FNodeCategory
	{
		UScriptStruct* StructBase = nullptr;
		UClass* BlueprintBase = nullptr;
	};

	FNodeCategory MakeTaskCategory()
	{
		return { FStateTreeTaskBase::StaticStruct(), UStateTreeTaskBlueprintBase::StaticClass() };
	}
	FNodeCategory MakeConditionCategory()
	{
		return { FStateTreeConditionBase::StaticStruct(), UStateTreeConditionBlueprintBase::StaticClass() };
	}
	FNodeCategory MakeConsiderationCategory()
	{
		return { FStateTreeConsiderationBase::StaticStruct(), UStateTreeConsiderationBlueprintBase::StaticClass() };
	}
	FNodeCategory MakeEvaluatorCategory()
	{
		return { FStateTreeEvaluatorBase::StaticStruct(), UStateTreeEvaluatorBlueprintBase::StaticClass() };
	}

	/** 解析后的节点定义。 */
	struct FNodeDefinition
	{
		UScriptStruct* Struct = nullptr;
		UClass* Class = nullptr;
		FString Name;
		TSharedPtr<FJsonObject> Props;
	};

	bool ParseNodeDefinition(
		const TSharedPtr<FJsonObject>& InJson,
		const FNodeCategory& InCategory,
		const FString& InFieldPath,
		FNodeDefinition& OutDefinition,
		FForgeReport& OutReport)
	{
		FString Type;
		if (!InJson.IsValid() || !InJson->TryGetStringField(Field::Type, Type))
		{
			OutReport.AddError(InFieldPath, TEXT("节点缺少 type"));
			return false;
		}

		FString TypeError;
		if (!StateTreeCodec::ResolveNodeType(Type, OutDefinition.Struct, OutDefinition.Class, TypeError))
		{
			OutReport.AddError(FString::Printf(TEXT("%s.type"), *InFieldPath), TypeError);
			return false;
		}

		const bool bTypeAllowed = OutDefinition.Struct != nullptr
			? OutDefinition.Struct->IsChildOf(InCategory.StructBase)
			: OutDefinition.Class->IsChildOf(InCategory.BlueprintBase);
		if (!bTypeAllowed)
		{
			OutReport.AddError(
				FString::Printf(TEXT("%s.type"), *InFieldPath),
				FString::Printf(TEXT("`%s` 不是该列表允许的节点类型"), *Type));
			return false;
		}

		InJson->TryGetStringField(Field::Name, OutDefinition.Name);
		if (!OutDefinition.Name.IsEmpty()
			&& !ValidateConfigName(OutDefinition.Name, FString::Printf(TEXT("%s.name"), *InFieldPath), OutReport))
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (InJson->TryGetObjectField(Field::Props, Props) && Props->IsValid())
		{
			OutDefinition.Props = *Props;
		}

		return true;
	}

	const UStruct* GetDefinitionStruct(const FNodeDefinition& InDefinition)
	{
		// UClass 与 UScriptStruct 是兄弟（都继承 UStruct），共同基类才是能持有的类型。
		return InDefinition.Struct != nullptr
			? static_cast<const UStruct*>(InDefinition.Struct)
			: static_cast<const UStruct*>(InDefinition.Class);
	}

	/** 校验一组节点定义（dry-run 与真跑共用同一门）。 */
	void ValidateNodeList(
		const TArray<TSharedPtr<FJsonValue>>& InDefinitions,
		const FNodeCategory& InCategory,
		const FString& InFieldPath,
		FForgeReport& OutReport)
	{
		TSet<FString> SeenNames;

		for (int32 Index = 0; Index < InDefinitions.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* Json = nullptr;
			const FString Path = FString::Printf(TEXT("%s[%d]"), *InFieldPath, Index);
			if (!InDefinitions[Index].IsValid()
				|| !InDefinitions[Index]->TryGetObject(Json)
				|| !Json->IsValid())
			{
				OutReport.AddError(Path, TEXT("节点定义必须是 JSON 对象"));
				continue;
			}

			FNodeDefinition Definition;
			if (!ParseNodeDefinition(*Json, InCategory, Path, Definition, OutReport))
			{
				continue;
			}

			if (!Definition.Name.IsEmpty())
			{
				bool bAlreadySeen = false;
				SeenNames.Add(Definition.Name, &bAlreadySeen);
				if (bAlreadySeen)
				{
					OutReport.AddError(
						Path,
						FString::Printf(TEXT("同一列表内出现重名节点 `%s`；引用会歧义"), *Definition.Name));
				}
			}

			if (const UStruct* PropsStruct = GetDefinitionStruct(Definition))
			{
				ForgePropertyCodec::ValidateProperties(
					PropsStruct,
					Definition.Props,
					OutReport.Diagnostics,
					FString::Printf(TEXT("%s.props"), *Path));
			}
		}
	}

	/** 初始化一个新节点（C++ 结构或蓝图 wrapper），Outer 归列表宿主。 */
	bool InitializeNewNode(
		FStateTreeEditorNode& OutNode,
		UObject& InOuter,
		const FNodeDefinition& InDefinition,
		FString& OutError)
	{
		if (InDefinition.Struct != nullptr)
		{
			OutNode.InitializeAs(&InOuter, InDefinition.Struct);
			return true;
		}

		if (InDefinition.Class == nullptr)
		{
			OutError = TEXT("节点类型为空");
			return false;
		}

		if (InDefinition.Class->IsChildOf(UStateTreeTaskBlueprintBase::StaticClass()))
		{
			FStateTreeBlueprintTaskWrapper Wrapper;
			Wrapper.TaskClass = InDefinition.Class;
			OutNode.InitializeAs<FStateTreeBlueprintTaskWrapper>(&InOuter, MoveTemp(Wrapper));
			return true;
		}
		if (InDefinition.Class->IsChildOf(UStateTreeEvaluatorBlueprintBase::StaticClass()))
		{
			FStateTreeBlueprintEvaluatorWrapper Wrapper;
			Wrapper.EvaluatorClass = InDefinition.Class;
			OutNode.InitializeAs<FStateTreeBlueprintEvaluatorWrapper>(&InOuter, MoveTemp(Wrapper));
			return true;
		}
		if (InDefinition.Class->IsChildOf(UStateTreeConditionBlueprintBase::StaticClass()))
		{
			FStateTreeBlueprintConditionWrapper Wrapper;
			Wrapper.ConditionClass = InDefinition.Class;
			OutNode.InitializeAs<FStateTreeBlueprintConditionWrapper>(&InOuter, MoveTemp(Wrapper));
			return true;
		}
		if (InDefinition.Class->IsChildOf(UStateTreeConsiderationBlueprintBase::StaticClass()))
		{
			FStateTreeBlueprintConsiderationWrapper Wrapper;
			Wrapper.ConsiderationClass = InDefinition.Class;
			OutNode.InitializeAs<FStateTreeBlueprintConsiderationWrapper>(&InOuter, MoveTemp(Wrapper));
			return true;
		}

		OutError = FString::Printf(TEXT("`%s` 不是状态树蓝图基类的派生类"), *InDefinition.Class->GetName());
		return false;
	}

	/**
	 * 把配置属性写进节点的「实例值层」（编译读取的那一层）。
	 *
	 * `Node` 模板也同步写一份：编辑器 Details 同时展示两层，且"重置"会从模板取值——
	 * 让两层保持一致，无论从哪一层看都与配置相同。
	 */
	void SyncNodeProps(
		FStateTreeEditorNode& InOutNode,
		UObject& InOuter,
		const FNodeDefinition& InDefinition,
		FForgeReport& OutReport,
		const FString& InFieldPath)
	{
		InOutNode.SetNodeName(InDefinition.Name.IsEmpty() ? NAME_None : FName(*InDefinition.Name));

		const FString PropsPath = InFieldPath + TEXT(".props");

		if (const UScriptStruct* InstanceStruct = Cast<const UScriptStruct>(InOutNode.GetInstance().GetStruct()))
		{
			void* InstanceMemory = const_cast<uint8*>(static_cast<const uint8*>(InOutNode.GetInstance().GetMemory()));
			ForgePropertyCodec::ResetStructToDefaults(InstanceStruct, InstanceMemory);
			if (InDefinition.Props.IsValid())
			{
				ForgePropertyCodec::ImportStructProperties(
					InstanceStruct, InstanceMemory, InDefinition.Props, OutReport.Diagnostics, PropsPath, &InOuter);
			}

			// 只有「节点结构就是实例数据」（GetInstanceDataType 返回自身）时才双写模板：
			// 独立 InstanceData 的节点（如并行状态树任务）身上根本没有那些属性，写了会误报。
			const UScriptStruct* NodeStruct = InOutNode.GetNode().GetScriptStruct();
			if (NodeStruct != nullptr && NodeStruct == InstanceStruct)
			{
				void* NodeMemory = const_cast<uint8*>(static_cast<const uint8*>(InOutNode.GetNode().GetMemory()));
				ForgePropertyCodec::ResetStructToDefaults(NodeStruct, NodeMemory);
				if (InDefinition.Props.IsValid())
				{
					ForgePropertyCodec::ImportStructProperties(
						NodeStruct, NodeMemory, InDefinition.Props, OutReport.Diagnostics, PropsPath, &InOuter);
				}
			}
			return;
		}

		if (UObject* InstanceObject = InOutNode.InstanceObject)
		{
			ForgePropertyCodec::ResetToDefaults(InstanceObject);
			if (InDefinition.Props.IsValid())
			{
				ForgePropertyCodec::ImportProperties(InstanceObject, InDefinition.Props, OutReport.Diagnostics, PropsPath);
			}
		}
	}

	/**
	 * 同步一组编辑节点：名字优先、位置兜底复用；配置未写的删除。
	 *
	 * 「名字」是节点自身的显式名（`FStateTreeNodeBase::Name`，即上次导入留下的名字）；
	 * 位置兜底让「给未命名节点补名字」这种编辑不会重建节点。
	 */
	void SyncNodeList(
		UObject& InOuter,
		TArray<FStateTreeEditorNode>& InOutNodes,
		const TArray<TSharedPtr<FJsonValue>>& InDefinitions,
		const FNodeCategory& InCategory,
		FForgeReport& OutReport,
		const FString& InFieldPath)
	{
		TMap<FString, int32> ExistingByName;
		TArray<bool> Used;
		Used.Init(false, InOutNodes.Num());
		for (int32 Index = 0; Index < InOutNodes.Num(); ++Index)
		{
			if (const FStateTreeNodeBase* NodeBase = InOutNodes[Index].GetNode().GetPtr<FStateTreeNodeBase>())
			{
				if (!NodeBase->Name.IsNone())
				{
					ExistingByName.Add(NodeBase->Name.ToString(), Index);
				}
			}
		}

		TArray<FStateTreeEditorNode> NewNodes;
		NewNodes.Reserve(InDefinitions.Num());

		for (int32 Index = 0; Index < InDefinitions.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* Json = nullptr;
			const FString Path = FString::Printf(TEXT("%s[%d]"), *InFieldPath, Index);
			if (!InDefinitions[Index].IsValid()
				|| !InDefinitions[Index]->TryGetObject(Json)
				|| !Json->IsValid())
			{
				continue;
			}

			FNodeDefinition Definition;
			if (!ParseNodeDefinition(*Json, InCategory, Path, Definition, OutReport))
			{
				continue;
			}

			int32 MatchIndex = INDEX_NONE;
			if (!Definition.Name.IsEmpty())
			{
				if (const int32* Found = ExistingByName.Find(Definition.Name))
				{
					if (!Used[*Found]
						&& StateTreeCodec::IsNodeTypeMatch(InOutNodes[*Found], Definition.Struct, Definition.Class))
					{
						MatchIndex = *Found;
					}
				}
			}
			if (MatchIndex == INDEX_NONE
				&& InOutNodes.IsValidIndex(Index)
				&& !Used[Index]
				&& StateTreeCodec::IsNodeTypeMatch(InOutNodes[Index], Definition.Struct, Definition.Class))
			{
				MatchIndex = Index;
			}

			if (MatchIndex != INDEX_NONE)
			{
				Used[MatchIndex] = true;
				SyncNodeProps(InOutNodes[MatchIndex], InOuter, Definition, OutReport, Path);
				NewNodes.Add(MoveTemp(InOutNodes[MatchIndex]));
			}
			else
			{
				FStateTreeEditorNode NewNode;
				FString Error;
				if (!InitializeNewNode(NewNode, InOuter, Definition, Error))
				{
					OutReport.AddError(Path, Error);
					continue;
				}
				SyncNodeProps(NewNode, InOuter, Definition, OutReport, Path);
				NewNodes.Add(MoveTemp(NewNode));
			}
		}

		InOutNodes = MoveTemp(NewNodes);
	}

	// ---- 计划（自算：`ForgeDiff` 的内容比较不含 Extra，而状态的内容都在 Extra 里）----

	FString ToCanonicalValueString(const TSharedPtr<FJsonValue>& InValue)
	{
		if (!InValue.IsValid())
		{
			return FString();
		}

		const TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
		Wrapper->SetField(TEXT("v"), InValue);
		return ForgeJsonText::ToCanonicalString(Wrapper);
	}

	/** 深拷贝（经 canonical 文本往返；只用于比较路径，不动原文档）。 */
	TSharedPtr<FJsonObject> DeepCopyObject(const TSharedPtr<FJsonObject>& InObject)
	{
		if (!InObject.IsValid())
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Copy;
		FString Error;
		if (!ForgeJsonText::ParseObject(ForgeJsonText::ToCanonicalString(InObject), Copy, Error))
		{
			return nullptr;
		}
		return Copy;
	}

	void RemoveFieldIfDefaultBool(const TSharedPtr<FJsonObject>& InOutObject, const TCHAR* InField, const bool bInDefault)
	{
		const TSharedPtr<FJsonValue> Value = InOutObject->TryGetField(InField);
		if (Value.IsValid() && Value->Type == EJson::Boolean && Value->AsBool() == bInDefault)
		{
			InOutObject->RemoveField(InField);
		}
	}

	void RemoveFieldIfDefaultString(const TSharedPtr<FJsonObject>& InOutObject, const TCHAR* InField, const TCHAR* InDefault)
	{
		const TSharedPtr<FJsonValue> Value = InOutObject->TryGetField(InField);
		if (Value.IsValid() && Value->Type == EJson::String && Value->AsString() == InDefault)
		{
			InOutObject->RemoveField(InField);
		}
	}

	void RemoveFieldIfDefaultNumber(const TSharedPtr<FJsonObject>& InOutObject, const TCHAR* InField, const double InDefault)
	{
		const TSharedPtr<FJsonValue> Value = InOutObject->TryGetField(InField);
		if (Value.IsValid() && Value->Type == EJson::Number && FMath::IsNearlyEqual(Value->AsNumber(), InDefault))
		{
			InOutObject->RemoveField(InField);
		}
	}

	/** 节点对象（任务 / 条件 / 评估器…）里的默认字段。 */
	void StripNodeDefaults(const TSharedPtr<FJsonObject>& InOutNode)
	{
		RemoveFieldIfDefaultString(InOutNode, Field::Name, TEXT(""));
		const TSharedPtr<FJsonValue> Props = InOutNode->TryGetField(Field::Props);
		if (Props.IsValid() && Props->Type == EJson::Object && Props->AsObject()->Values.Num() == 0)
		{
			InOutNode->RemoveField(Field::Props);
		}
	}

	/** 转移条目里的默认字段。 */
	void StripTransitionDefaults(const TSharedPtr<FJsonObject>& InOutTransition)
	{
		RemoveFieldIfDefaultString(InOutTransition, Field::Priority, TEXT("Normal"));
		RemoveFieldIfDefaultBool(InOutTransition, Field::Enabled, true);
		RemoveFieldIfDefaultNumber(InOutTransition, Field::Delay, 0.0);
		RemoveFieldIfDefaultNumber(InOutTransition, Field::DelayVariance, 0.0);

		const TArray<TSharedPtr<FJsonValue>>* Conditions = nullptr;
		if (InOutTransition->TryGetArrayField(Field::Conditions, Conditions) && Conditions != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Condition : *Conditions)
			{
				if (Condition.IsValid() && Condition->Type == EJson::Object)
				{
					StripNodeDefaults(Condition->AsObject());
				}
			}
		}
	}

	/**
	 * 状态对象的比较归一：删掉等于默认值的字段，让「显式写了默认值」与「省略」在计划里等价
	 * （导入本身对两者一视同仁，计划判据不能更严）。
	 * 这张默认值表必须与 `SyncStateFields` 的读取默认保持一致——两边不一致会让收敛判据抖动。
	 */
	void StripStateDefaults(const TSharedPtr<FJsonObject>& InOutState)
	{
		if (!InOutState.IsValid())
		{
			return;
		}

		RemoveFieldIfDefaultString(InOutState, Field::Type, TEXT("State"));
		RemoveFieldIfDefaultString(InOutState, Field::Selection, TEXT("TrySelectChildrenInOrder"));
		RemoveFieldIfDefaultString(InOutState, Field::TasksCompletion, TEXT("Any"));
		RemoveFieldIfDefaultString(InOutState, Field::Description, TEXT(""));
		RemoveFieldIfDefaultString(InOutState, Field::Tag, TEXT(""));
		RemoveFieldIfDefaultNumber(InOutState, Field::Weight, 1.0);
		RemoveFieldIfDefaultBool(InOutState, Field::Enabled, true);
		RemoveFieldIfDefaultBool(InOutState, Field::CopyParameterBindingsOnTick, false);
		RemoveFieldIfDefaultBool(InOutState, Field::CheckPrerequisites, true);

		const TSharedPtr<FJsonValue> Parameters = InOutState->TryGetField(Field::Parameters);
		if (Parameters.IsValid() && Parameters->Type == EJson::Object && Parameters->AsObject()->Values.Num() == 0)
		{
			InOutState->RemoveField(Field::Parameters);
		}

		auto StripNodeArray = [](const TCHAR* InField, const TSharedPtr<FJsonObject>& InObject)
		{
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (InObject->TryGetArrayField(InField, Array) && Array != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& Item : *Array)
				{
					if (Item.IsValid() && Item->Type == EJson::Object)
					{
						StripNodeDefaults(Item->AsObject());
					}
				}
			}
		};

		StripNodeArray(Field::Tasks, InOutState);
		StripNodeArray(Field::EnterConditions, InOutState);
		StripNodeArray(Field::Considerations, InOutState);

		const TSharedPtr<FJsonValue> SingleTask = InOutState->TryGetField(Field::SingleTask);
		if (SingleTask.IsValid() && SingleTask->Type == EJson::Object)
		{
			StripNodeDefaults(SingleTask->AsObject());
		}

		const TArray<TSharedPtr<FJsonValue>>* Transitions = nullptr;
		if (InOutState->TryGetArrayField(Field::Transitions, Transitions) && Transitions != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Transition : *Transitions)
			{
				if (Transition.IsValid() && Transition->Type == EJson::Object)
				{
					StripTransitionDefaults(Transition->AsObject());
				}
			}
		}
	}

	bool AreExtraEqual(const TSharedPtr<FJsonObject>& InLeft, const TSharedPtr<FJsonObject>& InRight)
	{
		const TSharedPtr<FJsonObject> Left = DeepCopyObject(InLeft);
		const TSharedPtr<FJsonObject> Right = DeepCopyObject(InRight);
		StripStateDefaults(Left);
		StripStateDefaults(Right);

		const bool bLeftEmpty = !Left.IsValid() || Left->Values.Num() == 0;
		const bool bRightEmpty = !Right.IsValid() || Right->Values.Num() == 0;
		if (bLeftEmpty && bRightEmpty)
		{
			return true;
		}

		return ForgeJsonText::ToCanonicalString(Left) == ForgeJsonText::ToCanonicalString(Right);
	}

	void AddCreateSubtree(const FForgeNode& InNode, const FString& InNodePath, TArray<FForgePlanEntry>& OutPlan)
	{
		OutPlan.Add({ EForgeAction::Create, InNodePath, InNode.Name, FString() });
		for (const FForgeNode& Child : InNode.Children)
		{
			AddCreateSubtree(Child, FString::Printf(TEXT("%s/%s"), *InNodePath, *Child.Name), OutPlan);
		}
	}

	void ComputeStatePlan(
		const FForgeNode& InTarget,
		const FForgeNode& InCurrent,
		const FString& InNodePath,
		TArray<FForgePlanEntry>& OutPlan)
	{
		if (!AreExtraEqual(InTarget.Extra, InCurrent.Extra))
		{
			OutPlan.Add({
				EForgeAction::Update,
				InNodePath,
				InTarget.Name,
				ForgeDiff::DescribeObjectDiff(InTarget.Extra, InCurrent.Extra, TEXT("extra")) });
		}

		TMap<FString, const FForgeNode*> CurrentByName;
		for (const FForgeNode& Child : InCurrent.Children)
		{
			CurrentByName.Add(Child.Name, &Child);
		}

		TSet<FString> TargetNames;
		TArray<FString> TargetOrder;
		for (const FForgeNode& Child : InTarget.Children)
		{
			TargetNames.Add(Child.Name);
			TargetOrder.Add(Child.Name);

			const FString ChildPath = FString::Printf(TEXT("%s/%s"), *InNodePath, *Child.Name);
			if (const FForgeNode* const* Existing = CurrentByName.Find(Child.Name))
			{
				ComputeStatePlan(Child, **Existing, ChildPath, OutPlan);
			}
			else
			{
				AddCreateSubtree(Child, ChildPath, OutPlan);
			}
		}

		TArray<FString> CurrentOrder;
		for (const FForgeNode& Child : InCurrent.Children)
		{
			if (TargetNames.Contains(Child.Name))
			{
				CurrentOrder.Add(Child.Name);
			}
			else
			{
				OutPlan.Add({
					EForgeAction::Delete,
					FString::Printf(TEXT("%s/%s"), *InNodePath, *Child.Name),
					Child.Name,
					TEXT("配置中已移除") });
			}
		}

		if (CurrentOrder != TargetOrder)
		{
			OutPlan.Add({
				EForgeAction::Reorder,
				InNodePath,
				InTarget.Name,
				FString::Printf(
					TEXT("子状态顺序: [%s] → [%s]"),
					*FString::Join(CurrentOrder, TEXT(", ")),
					*FString::Join(TargetOrder, TEXT(", "))) });
		}
	}

	/** 顶层字段（树参数 / 评估器 / 全局任务 / 绑定）的差异 → 计划条目。 */
	void ComputeTopLevelPlan(const FForgeDocument& InTarget, const FForgeDocument& InCurrent, TArray<FForgePlanEntry>& OutPlan)
	{
		// 与状态层同一原则：比较按「导入读到的效果」宽松等价（空对象 / 空数组 = 缺省，
		// 全局任务完成方式的默认值 = 省略），否则配置的合法写法会让每次导入都产生虚假更新。
		auto NormalizeTopLevelValue = [](const TCHAR* InField, const TSharedPtr<FJsonValue>& InValue) -> TSharedPtr<FJsonValue>
		{
			if (!InValue.IsValid())
			{
				return nullptr;
			}
			if (InValue->Type == EJson::Object && InValue->AsObject()->Values.Num() == 0)
			{
				return nullptr;
			}
			if (InValue->Type == EJson::Array && InValue->AsArray().Num() == 0)
			{
				return nullptr;
			}
			if (FCString::Stricmp(InField, Field::GlobalTasksCompletion) == 0
				&& InValue->Type == EJson::String && InValue->AsString() == TEXT("Any"))
			{
				return nullptr;
			}
			return InValue;
		};

		auto CompareField = [&OutPlan, &NormalizeTopLevelValue](
			const FForgeDocument& A,
			const FForgeDocument& B,
			const TCHAR* InField,
			const TCHAR* InLabel)
		{
			TSharedPtr<FJsonValue> Left;
			TSharedPtr<FJsonValue> Right;
			if (A.Extra.IsValid())
			{
				Left = NormalizeTopLevelValue(InField, A.Extra->TryGetField(InField));
			}
			if (B.Extra.IsValid())
			{
				Right = NormalizeTopLevelValue(InField, B.Extra->TryGetField(InField));
			}

			const FString LeftText = ToCanonicalValueString(Left);
			const FString RightText = ToCanonicalValueString(Right);
			if (LeftText != RightText)
			{
				OutPlan.Add({
					EForgeAction::Update,
					InField,
					FString(),
					FString::Printf(TEXT("%s 与配置不一致（将整体重建）"), InLabel) });
			}
		};

		// 绑定按集合语义比较（配对集合的排序文本）；条目顺序不该算差异。
		auto BindingsKey = [](const FForgeDocument& Doc) -> FString
		{
			const TSharedPtr<FJsonValue> Value = Doc.Extra.IsValid() ? Doc.Extra->TryGetField(Field::Bindings) : nullptr;
			if (!Value.IsValid() || Value->Type != EJson::Array)
			{
				return FString();
			}

			TArray<FString> Keys;
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				if (!Item.IsValid() || Item->Type != EJson::Object)
				{
					continue;
				}
				const TSharedPtr<FJsonObject> Object = Item->AsObject();
				const TSharedPtr<FJsonValue> Source = Object->TryGetField(Field::Source);
				const TSharedPtr<FJsonValue> Target = Object->TryGetField(Field::Target);
				Keys.Add(FString::Printf(TEXT("%s|%s"),
					Source.IsValid() ? *Source->AsString() : TEXT(""),
					Target.IsValid() ? *Target->AsString() : TEXT("")));
			}
			Keys.Sort();
			return FString::Join(Keys, TEXT("\n"));
		};

		CompareField(InTarget, InCurrent, Field::Parameters, TEXT("树参数"));
		CompareField(InTarget, InCurrent, Field::Evaluators, TEXT("评估器"));
		CompareField(InTarget, InCurrent, Field::GlobalTasks, TEXT("全局任务"));
		CompareField(InTarget, InCurrent, Field::GlobalTasksCompletion, TEXT("全局任务完成方式"));

		if (BindingsKey(InTarget) != BindingsKey(InCurrent))
		{
			OutPlan.Add({
				EForgeAction::Update,
				Field::Bindings,
				FString(),
				TEXT("绑定 与配置不一致（将整体重建）") });
		}
	}

	// ---- 校验（dry-run 与真跑共用）----

	void ValidateTransitions(
		const TSharedPtr<FJsonObject>& InExtra,
		const FString& InFieldPath,
		FForgeReport& OutReport)
	{
		const TArray<TSharedPtr<FJsonValue>>* Transitions = nullptr;
		if (!InExtra.IsValid() || !InExtra->TryGetArrayField(Field::Transitions, Transitions) || Transitions == nullptr)
		{
			return;
		}

		for (int32 Index = 0; Index < Transitions->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* TransitionJson = nullptr;
			const FString Path = FString::Printf(TEXT("%s.transitions[%d]"), *InFieldPath, Index);
			if (!(*Transitions)[Index].IsValid()
				|| !(*Transitions)[Index]->TryGetObject(TransitionJson)
				|| !TransitionJson->IsValid())
			{
				OutReport.AddError(Path, TEXT("转移定义必须是 JSON 对象"));
				continue;
			}

			FString Trigger;
			if (!(*TransitionJson)->TryGetStringField(Field::Trigger, Trigger) || Trigger.IsEmpty())
			{
				OutReport.AddError(Path + TEXT(".trigger"), TEXT("转移缺少 trigger"));
			}
			else
			{
				int64 Value = 0;
				FString Error;
				if (!StateTreeCodec::ParseEnumByName(StaticEnum<EStateTreeTransitionTrigger>(), Trigger, Value, Error))
				{
					OutReport.AddError(Path + TEXT(".trigger"), Error);
				}
			}

			FString To;
			if (!(*TransitionJson)->TryGetStringField(Field::To, To) || To.IsEmpty())
			{
				OutReport.AddError(Path + TEXT(".to"), TEXT("转移缺少 to"));
			}

			FString Priority;
			if ((*TransitionJson)->TryGetStringField(Field::Priority, Priority) && !Priority.IsEmpty())
			{
				int64 Value = 0;
				FString Error;
				if (!StateTreeCodec::ParseEnumByName(StaticEnum<EStateTreeTransitionPriority>(), Priority, Value, Error))
				{
					OutReport.AddError(Path + TEXT(".priority"), Error);
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* Conditions = nullptr;
			if ((*TransitionJson)->TryGetArrayField(Field::Conditions, Conditions) && Conditions != nullptr)
			{
				ValidateNodeList(*Conditions, MakeConditionCategory(), Path + TEXT(".conditions"), OutReport);
			}
		}
	}

	void ValidateStateRecursive(
		const FForgeNode& InState,
		const FString& InFieldPath,
		FForgeReport& OutReport)
	{
		ValidateConfigName(InState.Name, InFieldPath + TEXT(".name"), OutReport);

		const TSharedPtr<FJsonObject>& Extra = InState.Extra;

		EStateTreeStateType StateType;
		ReadEnumField(Extra, Field::Type, EStateTreeStateType::State, StateType, OutReport, InFieldPath);
		EStateTreeStateSelectionBehavior Selection;
		ReadEnumField(Extra, Field::Selection, EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder, Selection, OutReport, InFieldPath);
		EStateTreeTaskCompletionType Completion;
		ReadEnumField(Extra, Field::TasksCompletion, EStateTreeTaskCompletionType::Any, Completion, OutReport, InFieldPath);

		auto ValidateList = [&OutReport](const TSharedPtr<FJsonObject>& InExtra, const TCHAR* InField, const FNodeCategory& InCategory, const FString& InPath)
		{
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (InExtra.IsValid() && InExtra->TryGetArrayField(InField, Array) && Array != nullptr)
			{
				ValidateNodeList(*Array, InCategory, FString::Printf(TEXT("%s.%s"), *InPath, InField), OutReport);
			}
		};

		ValidateList(Extra, Field::EnterConditions, MakeConditionCategory(), InFieldPath);
		ValidateList(Extra, Field::Tasks, MakeTaskCategory(), InFieldPath);
		ValidateList(Extra, Field::Considerations, MakeConsiderationCategory(), InFieldPath);

		const TSharedPtr<FJsonObject>* SingleTask = nullptr;
		if (Extra.IsValid() && Extra->TryGetObjectField(Field::SingleTask, SingleTask) && SingleTask->IsValid())
		{
			ValidateNodeList(
				{ MakeShared<FJsonValueObject>(*SingleTask) },
				MakeTaskCategory(),
				InFieldPath + TEXT(".singleTask"),
				OutReport);
		}

		ValidateTransitions(Extra, InFieldPath, OutReport);

		TSet<FString> SiblingNames;
		for (const FForgeNode& Child : InState.Children)
		{
			bool bAlreadySeen = false;
			SiblingNames.Add(Child.Name, &bAlreadySeen);
			if (bAlreadySeen)
			{
				OutReport.AddError(
					FString::Printf(TEXT("%s/%s.name"), *InFieldPath, *Child.Name),
					FString::Printf(TEXT("同级出现重名状态 `%s`"), *Child.Name));
			}

			ValidateStateRecursive(Child, FString::Printf(TEXT("%s/%s"), *InFieldPath, *Child.Name), OutReport);
		}
	}

	// ---- 同步：第一遍（结构 / 字段 / 节点 / 参数）----

	/** 递归入口（`SyncStateChildren` 也要用它，先声明）。 */
	void SyncStateContents(
		UStateTreeState& InOutState,
		const FForgeNode& InTarget,
		FForgeReport& OutReport,
		const FString& InPath);

	void SyncStateFields(
		UStateTreeState& InOutState,
		const FForgeNode& InTarget,
		FForgeReport& OutReport,
		const FString& InPath)
	{
		const TSharedPtr<FJsonObject>& Extra = InTarget.Extra;

		InOutState.Name = FName(*InTarget.Name);
		if (InOutState.Parameters.ID.IsValid() == false)
		{
			InOutState.Parameters.ID = FGuid::NewGuid();
		}

		ReadEnumField(Extra, Field::Type, EStateTreeStateType::State, InOutState.Type, OutReport, InPath);
		ReadEnumField(Extra, Field::Selection, EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder, InOutState.SelectionBehavior, OutReport, InPath);
		ReadEnumField(Extra, Field::TasksCompletion, EStateTreeTaskCompletionType::Any, InOutState.TasksCompletion, OutReport, InPath);

		FString TagText;
		if (Extra.IsValid())
		{
			Extra->TryGetStringField(Field::Tag, TagText);
		}
		WriteGameplayTag(TagText, InOutState.Tag);

		FString Description;
		if (Extra.IsValid())
		{
			Extra->TryGetStringField(Field::Description, Description);
		}
		InOutState.Description = Description;

		double Number = 0.0;
		ReadNumberField(Extra, Field::Weight, 1.0, Number, OutReport, InPath);
		InOutState.Weight = static_cast<float>(Number);

		bool bBool = true;
		ReadBoolField(Extra, Field::Enabled, true, bBool, OutReport, InPath);
		InOutState.bEnabled = bBool;

		if (Extra.IsValid() && Extra->HasField(Field::CustomTickRate))
		{
			ReadNumberField(Extra, Field::CustomTickRate, 0.0, Number, OutReport, InPath);
			InOutState.CustomTickRate = static_cast<float>(Number);
			InOutState.bHasCustomTickRate = true;
		}
		else
		{
			InOutState.CustomTickRate = 0.0f;
			InOutState.bHasCustomTickRate = false;
		}

		ReadBoolField(Extra, Field::CopyParameterBindingsOnTick, false, bBool, OutReport, InPath);
		InOutState.bCopyParameterBindingsOnTick = bBool;

		ReadBoolField(Extra, Field::CheckPrerequisites, true, bBool, OutReport, InPath);
		InOutState.bCheckPrerequisitesWhenActivatingChildDirectly = bBool;

		FString LinkedAssetPath;
		if (Extra.IsValid())
		{
			Extra->TryGetStringField(Field::LinkedAsset, LinkedAssetPath);
		}
		if (LinkedAssetPath.IsEmpty())
		{
			InOutState.LinkedAsset = nullptr;
		}
		else if (UStateTree* Linked = FindStateTree(LinkedAssetPath))
		{
			InOutState.LinkedAsset = Linked;
		}
		else
		{
			OutReport.AddError(InPath + TEXT(".linkedAsset"), FString::Printf(TEXT("找不到状态树 `%s`"), *LinkedAssetPath));
		}

		InOutState.bHasRequiredEventToEnter = false;
		InOutState.RequiredEventToEnter = FStateTreeEventDesc();
		const TSharedPtr<FJsonObject>* RequiredEvent = nullptr;
		if (Extra.IsValid() && Extra->TryGetObjectField(Field::RequiredEvent, RequiredEvent) && RequiredEvent->IsValid())
		{
			InOutState.bHasRequiredEventToEnter = true;

			FString EventTag;
			(*RequiredEvent)->TryGetStringField(Field::Tag, EventTag);
			WriteGameplayTag(EventTag, InOutState.RequiredEventToEnter.Tag);

			FString PayloadPath;
			if ((*RequiredEvent)->TryGetStringField(Field::Payload, PayloadPath) && !PayloadPath.IsEmpty())
			{
				if (UScriptStruct* Payload = LoadObject<UScriptStruct>(nullptr, *PayloadPath))
				{
					InOutState.RequiredEventToEnter.PayloadStruct = Payload;
				}
				else
				{
					OutReport.AddError(
						InPath + TEXT(".requiredEvent.payload"),
						FString::Printf(TEXT("找不到结构体 `%s`"), *PayloadPath));
				}
			}

			bool bConsume = true;
			ReadBoolField(*RequiredEvent, Field::Consume, true, bConsume, OutReport, InPath + TEXT(".requiredEvent"));
			InOutState.RequiredEventToEnter.bConsumeEventOnSelect = bConsume;
		}

		// 参数：配置没写 = 清空。
		const TSharedPtr<FJsonObject>* Parameters = nullptr;
		if (Extra.IsValid() && Extra->TryGetObjectField(Field::Parameters, Parameters) && Parameters->IsValid())
		{
			StateTreeCodec::ImportPropertyBag(
				InOutState.Parameters.Parameters,
				*Parameters,
				OutReport.Diagnostics,
				InPath + TEXT(".parameters"));
		}
		else
		{
			StateTreeCodec::ImportPropertyBag(
				InOutState.Parameters.Parameters,
				nullptr,
				OutReport.Diagnostics,
				InPath + TEXT(".parameters"));
		}

		// 节点列表。
		{
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			Extra->TryGetArrayField(Field::EnterConditions, Array);
			SyncNodeList(
				InOutState,
				InOutState.EnterConditions,
				Array != nullptr ? *Array : EmptyNodeArray(),
				MakeConditionCategory(),
				OutReport,
				InPath + TEXT(".enterConditions"));
		}
		{
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			Extra->TryGetArrayField(Field::Tasks, Array);
			SyncNodeList(
				InOutState,
				InOutState.Tasks,
				Array != nullptr ? *Array : EmptyNodeArray(),
				MakeTaskCategory(),
				OutReport,
				InPath + TEXT(".tasks"));
		}
		{
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			Extra->TryGetArrayField(Field::Considerations, Array);
			SyncNodeList(
				InOutState,
				InOutState.Considerations,
				Array != nullptr ? *Array : EmptyNodeArray(),
				MakeConsiderationCategory(),
				OutReport,
				InPath + TEXT(".considerations"));
		}

		// 单任务（schema 单任务模式）。
		const TSharedPtr<FJsonObject>* SingleTask = nullptr;
		if (Extra.IsValid() && Extra->TryGetObjectField(Field::SingleTask, SingleTask) && SingleTask->IsValid())
		{
			FNodeDefinition Definition;
			if (ParseNodeDefinition(*SingleTask, MakeTaskCategory(), InPath + TEXT(".singleTask"), Definition, OutReport))
			{
				FString Error;
				if (!InitializeNewNode(InOutState.SingleTask, InOutState, Definition, Error))
				{
					OutReport.AddError(InPath + TEXT(".singleTask"), Error);
				}
				else
				{
					SyncNodeProps(InOutState.SingleTask, InOutState, Definition, OutReport, InPath + TEXT(".singleTask"));
				}
			}
		}
		else
		{
			InOutState.SingleTask = FStateTreeEditorNode();
		}
	}

	void SyncStateChildren(
		UStateTreeState& InOutState,
		const FForgeNode& InTarget,
		FForgeReport& OutReport,
		const FString& InPath)
	{
		TMap<FString, UStateTreeState*> ExistingByName;
		for (const TObjectPtr<UStateTreeState>& Child : InOutState.Children)
		{
			if (Child != nullptr)
			{
				ExistingByName.Add(Child->Name.ToString(), Child);
			}
		}

		TSet<UStateTreeState*> Used;
		TArray<TObjectPtr<UStateTreeState>> NewChildren;
		NewChildren.Reserve(InTarget.Children.Num());

		for (const FForgeNode& ChildTarget : InTarget.Children)
		{
			const FString ChildPath = FString::Printf(TEXT("%s/%s"), *InPath, *ChildTarget.Name);

			UStateTreeState* Child = nullptr;
			if (UStateTreeState** Found = ExistingByName.Find(ChildTarget.Name))
			{
				if (!Used.Contains(*Found))
				{
					Child = *Found;
					Used.Add(Child);
				}
			}
			if (Child == nullptr)
			{
				Child = NewObject<UStateTreeState>(&InOutState, FName(), RF_Transactional);
				Child->Parent = &InOutState;
			}

			SyncStateContents(*Child, ChildTarget, OutReport, ChildPath);
			NewChildren.Add(Child);
		}

		// 配置移除的子状态不进新数组（对象随 GC 回收）。
		InOutState.Children = MoveTemp(NewChildren);
	}

	void SyncStateContents(
		UStateTreeState& InOutState,
		const FForgeNode& InTarget,
		FForgeReport& OutReport,
		const FString& InPath)
	{
		SyncStateFields(InOutState, InTarget, OutReport, InPath);
		SyncStateChildren(InOutState, InTarget, OutReport, InPath);
	}

	// ---- 同步：第二遍（引用；依赖"整棵树已落定"）----

	void CollectChildrenRecursive(UStateTreeState& InState, TMap<FString, UStateTreeState*>& OutStates)
	{
		OutStates.Add(InState.GetPath(), &InState);
		for (const TObjectPtr<UStateTreeState>& Child : InState.Children)
		{
			if (Child != nullptr)
			{
				CollectChildrenRecursive(*Child, OutStates);
			}
		}
	}

	void CollectStatesByPath(UStateTreeEditorData& InEditorData, TMap<FString, UStateTreeState*>& OutStates)
	{
		for (const TObjectPtr<UStateTreeState>& Root : InEditorData.SubTrees)
		{
			if (Root != nullptr)
			{
				CollectChildrenRecursive(*Root, OutStates);
			}
		}
	}

	/** 转移目标：内建目标（Succeeded 等）或状态路径。 */
	bool ResolveStateLinkTarget(
		const FString& InTo,
		const TMap<FString, UStateTreeState*>& InStatesByPath,
		FStateTreeStateLink& OutLink,
		EStateTreeTransitionType& OutType,
		FString& OutError)
	{
		static const TMap<FString, EStateTreeTransitionType> BuiltIn = {
			{ TEXT("Succeeded"), EStateTreeTransitionType::Succeeded },
			{ TEXT("Failed"), EStateTreeTransitionType::Failed },
			{ TEXT("Parent"), EStateTreeTransitionType::Parent },
			{ TEXT("NextState"), EStateTreeTransitionType::NextState },
			{ TEXT("NextSelectableState"), EStateTreeTransitionType::NextSelectableState },
			{ TEXT("NextParent"), EStateTreeTransitionType::NextParent },
			{ TEXT("NextSelectableParent"), EStateTreeTransitionType::NextSelectableParent },
		};

		if (const EStateTreeTransitionType* Found = BuiltIn.Find(InTo))
		{
			OutType = *Found;
			OutLink = FStateTreeStateLink(*Found);
			return true;
		}

		if (UStateTreeState* const* Target = InStatesByPath.Find(InTo))
		{
			OutType = EStateTreeTransitionType::GotoState;
			OutLink = (*Target)->GetLinkToState();
			return true;
		}

		OutError = FString::Printf(
			TEXT("`%s` 既不是内建目标（Succeeded / Failed / Parent / NextState / NextSelectableState / NextParent / NextSelectableParent），也不是配置中的状态路径"),
			*InTo);
		return false;
	}

	void SyncStateReferences(
		UStateTreeState& InOutState,
		const FForgeNode& InTarget,
		const TMap<FString, UStateTreeState*>& InStatesByPath,
		FForgeReport& OutReport,
		const FString& InPath)
	{
		const TSharedPtr<FJsonObject>& Extra = InTarget.Extra;

		// linkedSubtree（Linked 状态）。
		if (InOutState.Type == EStateTreeStateType::Linked)
		{
			FString SubtreePath;
			if (Extra.IsValid())
			{
				Extra->TryGetStringField(Field::LinkedSubtree, SubtreePath);
			}
			if (SubtreePath.IsEmpty())
			{
				OutReport.AddError(InPath + TEXT(".linkedSubtree"), TEXT("Linked 状态需要 linkedSubtree（要链接的子树状态路径）"));
			}
			else if (UStateTreeState* const* Target = InStatesByPath.Find(SubtreePath))
			{
				InOutState.LinkedSubtree = (*Target)->GetLinkToState();
			}
			else
			{
				OutReport.AddError(InPath + TEXT(".linkedSubtree"), FString::Printf(TEXT("找不到子树状态 `%s`"), *SubtreePath));
			}
		}
		else
		{
			InOutState.LinkedSubtree = FStateTreeStateLink();
		}

		// 转移：整体重建（转移没有跨次导入的引用身份；指向它的绑定也随全量重建重解析）。
		InOutState.Transitions.Reset();

		const TArray<TSharedPtr<FJsonValue>>* Transitions = nullptr;
		if (!Extra.IsValid() || !Extra->TryGetArrayField(Field::Transitions, Transitions) || Transitions == nullptr)
		{
			return;
		}

		for (int32 Index = 0; Index < Transitions->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* Json = nullptr;
			const FString Path = FString::Printf(TEXT("%s.transitions[%d]"), *InPath, Index);
			if (!(*Transitions)[Index].IsValid()
				|| !(*Transitions)[Index]->TryGetObject(Json)
				|| !Json->IsValid())
			{
				continue;
			}

			FString TriggerText;
			FString ToText;
			(*Json)->TryGetStringField(Field::Trigger, TriggerText);
			(*Json)->TryGetStringField(Field::To, ToText);

			int64 TriggerValue = 0;
			FString TriggerError;
			if (!StateTreeCodec::ParseEnumByName(StaticEnum<EStateTreeTransitionTrigger>(), TriggerText, TriggerValue, TriggerError))
			{
				OutReport.AddError(Path + TEXT(".trigger"), TriggerError);
				continue;
			}

			FStateTreeStateLink Link;
			EStateTreeTransitionType LinkType = EStateTreeTransitionType::None;
			FString LinkError;
			if (!ResolveStateLinkTarget(ToText, InStatesByPath, Link, LinkType, LinkError))
			{
				OutReport.AddError(Path + TEXT(".to"), LinkError);
				continue;
			}

			// 事件（OnEvent 的 tag 在构造时传入；consume 与 payload 随后覆盖）。
			const TSharedPtr<FJsonObject>* EventJson = nullptr;
			FGameplayTag EventTag;
			if ((*Json)->TryGetObjectField(Field::Event, EventJson) && EventJson->IsValid())
			{
				FString EventTagText;
				(*EventJson)->TryGetStringField(Field::Tag, EventTagText);
				WriteGameplayTag(EventTagText, EventTag);
			}

			const UStateTreeState* GotoTarget = nullptr;
			if (LinkType == EStateTreeTransitionType::GotoState)
			{
				GotoTarget = InStatesByPath.FindRef(ToText);
			}

			FStateTreeTransition& Transition = InOutState.AddTransition(
				static_cast<EStateTreeTransitionTrigger>(TriggerValue),
				EventTag,
				LinkType,
				GotoTarget);

			if (LinkType == EStateTreeTransitionType::GotoState)
			{
				Transition.State = Link;
			}

			if (EventJson != nullptr && EventJson->IsValid())
			{
				bool bConsume = true;
				ReadBoolField(*EventJson, Field::Consume, true, bConsume, OutReport, Path + TEXT(".event"));
				Transition.RequiredEvent.bConsumeEventOnSelect = bConsume;

				FString PayloadPath;
				if ((*EventJson)->TryGetStringField(Field::Payload, PayloadPath) && !PayloadPath.IsEmpty())
				{
					if (UScriptStruct* Payload = LoadObject<UScriptStruct>(nullptr, *PayloadPath))
					{
						Transition.RequiredEvent.PayloadStruct = Payload;
					}
					else
					{
						OutReport.AddError(Path + TEXT(".event.payload"), FString::Printf(TEXT("找不到结构体 `%s`"), *PayloadPath));
					}
				}
			}

			FString PriorityText;
			if ((*Json)->TryGetStringField(Field::Priority, PriorityText) && !PriorityText.IsEmpty())
			{
				int64 PriorityValue = 0;
				FString PriorityError;
				if (StateTreeCodec::ParseEnumByName(StaticEnum<EStateTreeTransitionPriority>(), PriorityText, PriorityValue, PriorityError))
				{
					Transition.Priority = static_cast<EStateTreeTransitionPriority>(PriorityValue);
				}
				else
				{
					OutReport.AddError(Path + TEXT(".priority"), PriorityError);
				}
			}
			else
			{
				Transition.Priority = EStateTreeTransitionPriority::Normal;
			}

			if ((*Json)->HasField(Field::Delay))
			{
				double DelayValue = 0.0;
				double VarianceValue = 0.0;
				ReadNumberField(*Json, Field::Delay, 0.0, DelayValue, OutReport, Path);
				ReadNumberField(*Json, Field::DelayVariance, 0.0, VarianceValue, OutReport, Path);
				Transition.bDelayTransition = true;
				Transition.DelayDuration = static_cast<float>(DelayValue);
				Transition.DelayRandomVariance = static_cast<float>(VarianceValue);
			}
			else
			{
				Transition.bDelayTransition = false;
				Transition.DelayDuration = 0.0f;
				Transition.DelayRandomVariance = 0.0f;
			}

			bool bEnabled = true;
			ReadBoolField(*Json, Field::Enabled, true, bEnabled, OutReport, Path);
			Transition.bTransitionEnabled = bEnabled;

			const TArray<TSharedPtr<FJsonValue>>* Conditions = nullptr;
			if ((*Json)->TryGetArrayField(Field::Conditions, Conditions) && Conditions != nullptr)
			{
				SyncNodeList(InOutState, Transition.Conditions, *Conditions, MakeConditionCategory(), OutReport, Path + TEXT(".conditions"));
			}
			else
			{
				Transition.Conditions.Reset();
			}
		}
	}

	void SyncReferencesRecursive(
		UStateTreeState& InOutState,
		const FForgeNode& InTarget,
		const TMap<FString, UStateTreeState*>& InStatesByPath,
		FForgeReport& OutReport,
		const FString& InPath)
	{
		SyncStateReferences(InOutState, InTarget, InStatesByPath, OutReport, InPath);

		TMap<FString, UStateTreeState*> ChildrenByName;
		for (const TObjectPtr<UStateTreeState>& Child : InOutState.Children)
		{
			if (Child != nullptr)
			{
				ChildrenByName.Add(Child->Name.ToString(), Child);
			}
		}

		for (const FForgeNode& ChildTarget : InTarget.Children)
		{
			if (UStateTreeState* Child = ChildrenByName.FindRef(ChildTarget.Name))
			{
				SyncReferencesRecursive(
					*Child,
					ChildTarget,
					InStatesByPath,
					OutReport,
					FString::Printf(TEXT("%s/%s"), *InPath, *ChildTarget.Name));
			}
		}
	}

	// ---- 同步：顶层（评估器 / 全局任务 / 树参数 / 绑定）----

	void SyncGlobalNodes(
		UStateTreeEditorData& InOutEditorData,
		const FForgeDocument& InDocument,
		FForgeReport& OutReport)
	{
		const TSharedPtr<FJsonObject>& Extra = InDocument.Extra;

		// 树参数：配置没写 = 清空。
		const TSharedPtr<FJsonObject>* Parameters = nullptr;
		if (Extra.IsValid() && Extra->TryGetObjectField(Field::Parameters, Parameters) && Parameters->IsValid())
		{
			StateTreeCodec::ImportPropertyBag(
				const_cast<FInstancedPropertyBag&>(InOutEditorData.GetRootParametersPropertyBag()),
				*Parameters,
				OutReport.Diagnostics,
				TEXT("parameters"));
		}
		else
		{
			StateTreeCodec::ImportPropertyBag(
				const_cast<FInstancedPropertyBag&>(InOutEditorData.GetRootParametersPropertyBag()),
				nullptr,
				OutReport.Diagnostics,
				TEXT("parameters"));
		}

		{
			const TArray<TSharedPtr<FJsonValue>>* Evaluators = nullptr;
			if (Extra.IsValid())
			{
				Extra->TryGetArrayField(Field::Evaluators, Evaluators);
			}
			SyncNodeList(
				InOutEditorData,
				InOutEditorData.Evaluators,
				Evaluators != nullptr ? *Evaluators : EmptyNodeArray(),
				MakeEvaluatorCategory(),
				OutReport,
				TEXT("evaluators"));
		}
		{
			const TArray<TSharedPtr<FJsonValue>>* GlobalTasks = nullptr;
			if (Extra.IsValid())
			{
				Extra->TryGetArrayField(Field::GlobalTasks, GlobalTasks);
			}
			SyncNodeList(
				InOutEditorData,
				InOutEditorData.GlobalTasks,
				GlobalTasks != nullptr ? *GlobalTasks : EmptyNodeArray(),
				MakeTaskCategory(),
				OutReport,
				TEXT("globalTasks"));
		}

		ReadEnumField(
			Extra,
			Field::GlobalTasksCompletion,
			EStateTreeTaskCompletionType::Any,
			InOutEditorData.GlobalTasksCompletion,
			OutReport,
			TEXT("globalTasksCompletion"));
	}

	void SyncBindings(
		UStateTreeEditorData& InOutEditorData,
		const FForgeDocument& InDocument,
		FForgeReport& OutReport)
	{
		FStateTreeEditorPropertyBindings* Bindings = InOutEditorData.GetPropertyEditorBindings();
		if (Bindings == nullptr)
		{
			return;
		}

		// 全量重建：先清空（谓词版会同步依赖缓存）。
		Bindings->RemoveBindings([](FPropertyBindingBinding&)
		{
			return true;
		});

		const TSharedPtr<FJsonObject>& Extra = InDocument.Extra;
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Extra.IsValid() || !Extra->TryGetArrayField(Field::Bindings, Entries) || Entries == nullptr)
		{
			return;
		}

		for (int32 Index = 0; Index < Entries->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			const FString Path = FString::Printf(TEXT("bindings[%d]"), Index);
			if (!(*Entries)[Index].IsValid()
				|| !(*Entries)[Index]->TryGetObject(Entry)
				|| !Entry->IsValid())
			{
				OutReport.AddError(Path, TEXT("绑定定义必须是 JSON 对象"));
				continue;
			}

			FString Source;
			FString Target;
			(*Entry)->TryGetStringField(Field::Source, Source);
			(*Entry)->TryGetStringField(Field::Target, Target);

			FGuid SourceID;
			FString SourceProperty;
			FString Error;
			if (!StateTreeCodec::ResolveStructReference(InOutEditorData, Source, SourceID, SourceProperty, Error))
			{
				OutReport.AddError(Path + TEXT(".source"), Error);
				continue;
			}

			FGuid TargetID;
			FString TargetProperty;
			if (!StateTreeCodec::ResolveStructReference(InOutEditorData, Target, TargetID, TargetProperty, Error))
			{
				OutReport.AddError(Path + TEXT(".target"), Error);
				continue;
			}

			FPropertyBindingPath SourcePath;
			if (!SourceProperty.IsEmpty())
			{
				SourcePath.FromString(SourceProperty);
			}
			SourcePath.SetStructID(SourceID);

			FPropertyBindingPath TargetPath;
			if (!TargetProperty.IsEmpty())
			{
				TargetPath.FromString(TargetProperty);
			}
			TargetPath.SetStructID(TargetID);

			// `AddBinding` 无返回值；路径无效时引擎会拒绝或修剪，由编译与导出复核兜底。
			Bindings->AddBinding(SourcePath, TargetPath);
		}
	}

	// ---- 编译 ----

	bool RunStateTreeCompile(UStateTree& InTree, FForgeReport& OutReport)
	{
		UStateTreeEditingSubsystem::ValidateStateTree(&InTree);

		FStateTreeCompilerLog Log;
		const bool bCompiled = UStateTreeEditingSubsystem::CompileStateTree(&InTree, Log);

		for (const TSharedRef<FTokenizedMessage>& Message : Log.ToTokenizedMessages())
		{
			const EMessageSeverity::Type Severity = Message->GetSeverity();

			EForgeSeverity Mapped = EForgeSeverity::Info;
			if (Severity == EMessageSeverity::Error || Severity == EMessageSeverity::PerformanceWarning)
			{
				Mapped = EForgeSeverity::Error;
			}
			else if (Severity == EMessageSeverity::Warning)
			{
				Mapped = EForgeSeverity::Warning;
			}

			OutReport.Diagnostics.Emplace(Mapped, TEXT("compile"), Message->ToText().ToString());
		}

		if (!bCompiled)
		{
			OutReport.AddError(TEXT("compile"), TEXT("StateTree 编译失败，资产未保存"));
			return false;
		}

		return true;
	}

} // namespace

bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport)
{
	OutReport.AssetPath = InAssetPath;

	UStateTree* Tree = FindStateTree(InAssetPath);
	if (Tree == nullptr)
	{
		OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("找不到状态树：%s"), *InAssetPath));
		return false;
	}

	StateTreeCodec::ExportDocument(*Tree, OutDocument, OutReport.Diagnostics);
	if (OutReport.HasErrors())
	{
		return false;
	}

	OutReport.bSucceeded = true;
	return true;
}

bool ImportDocument(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport)
{
	OutReport.bDryRun = bDryRun;
	OutReport.SourcePath = InDocument.SourcePath;
	OutReport.AssetPath = InDocument.Asset;

	if (InDocument.Asset.IsEmpty())
	{
		OutReport.AddError(TEXT("asset"), TEXT("配置缺少 asset（目标资产路径）"));
		return false;
	}

	// ① Schema：导入必须有（它决定 EditorData 类与 context 结构）。
	FString SchemaPath;
	if (!InDocument.Extra.IsValid()
		|| !InDocument.Extra->TryGetStringField(Field::Schema, SchemaPath)
		|| SchemaPath.IsEmpty())
	{
		OutReport.AddError(
			TEXT("schema"),
			TEXT("配置缺少 schema（新建时用它选择资产类型，导入已有资产时用于一致性校验）"));
		return false;
	}

	UClass* SchemaClass = LoadClass<UStateTreeSchema>(nullptr, *SchemaPath);
	if (SchemaClass == nullptr)
	{
		OutReport.AddError(TEXT("schema"), FString::Printf(TEXT("找不到 Schema 类 `%s`"), *SchemaPath));
		return false;
	}

	// ② 结构校验（名字 / 枚举 / 节点 / 转移；dry-run 与真跑同一门）。
	ValidateStateRecursive(InDocument.Root, TEXT("root"), OutReport);

	// ③ 找资产、取当前快照、校验 schema 一致。
	UStateTree* Tree = FindStateTree(InDocument.Asset);

	FForgeDocument CurrentDocument;
	if (Tree != nullptr)
	{
		TArray<FForgeDiagnostic> SnapshotDiagnostics;
		StateTreeCodec::ExportDocument(*Tree, CurrentDocument, SnapshotDiagnostics);

		const UStateTreeEditorData* EditorData = Cast<const UStateTreeEditorData>(Tree->EditorData.Get());
		if (EditorData == nullptr)
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("%s 没有编辑器数据（EditorData），无法导入"), *InDocument.Asset));
			return false;
		}

		if (EditorData->Schema == nullptr || EditorData->Schema->GetClass() != SchemaClass)
		{
			OutReport.AddError(
				TEXT("schema"),
				FString::Printf(
					TEXT("schema 与资产不一致：资产是 `%s`，配置要求 `%s`；schema 决定 context 结构，不能悄悄换"),
					EditorData->Schema ? *EditorData->Schema->GetClass()->GetPathName() : TEXT("(无)"),
					*SchemaPath));
			return false;
		}
	}

	// ④ 计划：状态树（含 Extra 内容比较）+ 顶层字段。
	if (Tree != nullptr)
	{
		ComputeStatePlan(InDocument.Root, CurrentDocument.Root, TEXT("root"), OutReport.Actions);
	}
	else
	{
		AddCreateSubtree(InDocument.Root, TEXT("root"), OutReport.Actions);
	}

	ComputeTopLevelPlan(InDocument, CurrentDocument, OutReport.Actions);

	OutReport.CountActions();

	if (bDryRun)
	{
		OutReport.bSucceeded = !OutReport.HasErrors();
		return OutReport.bSucceeded;
	}

	if (OutReport.HasErrors())
	{
		return false;
	}

	if (Tree != nullptr && OutReport.Actions.Num() == 0)
	{
		OutReport.bSucceeded = true;
		return true;
	}

	// 真跑比 dry-run 多一层校验：schema 必须能实例化（dry-run 只做类解析，不建对象）。
	if (SchemaClass->HasAnyClassFlags(CLASS_Abstract))
	{
		OutReport.AddError(
			TEXT("schema"),
			FString::Printf(TEXT("`%s` 是抽象类，不能实例化；请写具体的 schema 子类"), *SchemaPath));
		return false;
	}

	// ⑤ 建资产（不存在时）：照 `UStateTreeFactory` 的初始化流程。
	const bool bCreated = Tree == nullptr;
	if (bCreated)
	{
		UPackage* Package = CreatePackage(*InDocument.Asset);
		if (Package == nullptr)
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("无法创建包：%s"), *InDocument.Asset));
			return false;
		}

		FString PackagePath;
		FString AssetName;
		InDocument.Asset.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (AssetName.IsEmpty())
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("资产路径 `%s` 不合法，应形如 /Game/AI/ST_Xxx"), *InDocument.Asset));
			return false;
		}

		Tree = NewObject<UStateTree>(Package, FName(*AssetName), RF_Public | RF_Standalone);
		if (Tree == nullptr)
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("创建状态树失败：%s"), *InDocument.Asset));
			return false;
		}
	}

	// `UStateTreeEditorData` 是 MinimalAPI 类，`GetEditorData` 未导出——直接读资产字段再 Cast。
	UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(Tree->EditorData.Get());
	if (EditorData == nullptr)
	{
		FStateTreeEditorModule& EditorModule = FStateTreeEditorModule::GetModule();
		const auto EditorDataClass = EditorModule.GetEditorDataClass(SchemaClass);
		EditorData = NewObject<UStateTreeEditorData>(Tree, EditorDataClass, FName(), RF_Transactional);
		Tree->EditorData = EditorData;
	}
	if (EditorData->Schema == nullptr)
	{
		EditorData->Schema = NewObject<UStateTreeSchema>(EditorData, SchemaClass, FName(), RF_Transactional);
	}
	if (EditorData->EditorSchema == nullptr)
	{
		FStateTreeEditorModule& EditorModule = FStateTreeEditorModule::GetModule();
		const auto EditorSchemaClass = EditorModule.GetEditorSchemaClass(SchemaClass);
		EditorData->EditorSchema = NewObject<UStateTreeEditorSchema>(EditorData, EditorSchemaClass, FName(), RF_Transactional);
	}

	// ⑥ 同步第一遍：结构 / 字段 / 节点 / 参数。
	{
		UStateTreeState* RootState = EditorData->SubTrees.Num() > 0 ? EditorData->SubTrees[0].Get() : nullptr;

		if (RootState == nullptr || RootState->Name.ToString() != InDocument.Root.Name)
		{
			// 改名 = 删旧建新（与子状态的语义一致）。
			UStateTreeState* NewRoot = NewObject<UStateTreeState>(EditorData, FName(), RF_Transactional);
			SyncStateContents(*NewRoot, InDocument.Root, OutReport, TEXT("root"));
			EditorData->SubTrees.Empty();
			EditorData->SubTrees.Add(NewRoot);
		}
		else
		{
			SyncStateContents(*RootState, InDocument.Root, OutReport, TEXT("root"));
		}
	}

	// 顶层：评估器 / 全局任务 / 树参数。
	SyncGlobalNodes(*EditorData, InDocument, OutReport);

	// ⑦ 同步第二遍：引用（linkedSubtree / 转移 / 绑定）——必须在整棵树落定之后。
	{
		TMap<FString, UStateTreeState*> StatesByPath;
		CollectStatesByPath(*EditorData, StatesByPath);

		UStateTreeState* RootState = EditorData->SubTrees.Num() > 0 ? EditorData->SubTrees[0].Get() : nullptr;
		if (RootState != nullptr)
		{
			SyncReferencesRecursive(*RootState, InDocument.Root, StatesByPath, OutReport, TEXT("root"));
		}

		SyncBindings(*EditorData, InDocument, OutReport);
	}

	if (OutReport.HasErrors())
	{
		return false;
	}

	// ⑧ 编译：失败不保存（资产文件保持原样）。
	if (!RunStateTreeCompile(*Tree, OutReport))
	{
		return false;
	}

	// ⑨ 保存。
	FString SaveError;
	if (!SaveAssetPackage(Tree, SaveError))
	{
		OutReport.AddError(TEXT("asset"), SaveError);
		return false;
	}

	if (bCreated)
	{
		FAssetRegistryModule::AssetCreated(Tree);
	}

	OutReport.bSucceeded = true;
	return true;
}

} // namespace StateTreeForge

#endif // ASSETFORGE_WITH_STATETREE
