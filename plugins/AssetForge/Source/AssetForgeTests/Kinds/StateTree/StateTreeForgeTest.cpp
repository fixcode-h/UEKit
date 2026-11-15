// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Kinds/StateTree/StateTreeCodec.h"
#include "Kinds/StateTree/StateTreeForge.h"
#include "Registry/ForgeKindRegistry.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgePipeline.h"
#include "Core/ForgeTypes.h"

#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeState.h"
#include "StateTreeTaskBase.h"
#include "StructUtils/PropertyBag.h"

#if WITH_AUTOMATION_WORKER

/**
 * 状态树处理器（state-tree）的守门用例。
 *
 * 覆盖「这个 kind 才知道、通用层给不了」的判定与转换：
 *
 * - 归类顺序：`UStateTree` 是 `UDataAsset` 的子类，必须先于 data-asset 分派；
 * - dry-run 校验：schema / 节点类型 / 名字规则 / 重名 / 转移字段；
 * - 引用解析：`状态路径/名字` 与引擎可绑定描述符完全对齐（含歧义拦截）；
 * - 导出快照：状态层级、任务、转移（GotoState 按 ID 反查为状态路径）；
 * - 参数（PropertyBag）的结构化读写与「配置未写即删除」。
 *
 * 走 dry-run 与内存态对象：不建包、不写盘——「导入落盘后的回环」由自检配置
 * 端到端负责（见 skill `tk-state-tree-config`）。
 */
TEST_CLASS_WITH_FLAGS(StateTreeForgeTest, "AssetForge.StateTree",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 状态节点补 `class`：契约层要求每个节点有它；状态固定用 UStateTreeState 的类路径。 */
	static void InjectStateClass(const TSharedPtr<FJsonObject>& InOutState)
	{
		if (!InOutState.IsValid())
		{
			return;
		}

		if (!InOutState->HasField(TEXT("class")))
		{
			// 状态对象本身的类（注意：UStateTreeState 在 StateTreeEditorModule，不在 StateTreeModule）。
			InOutState->SetStringField(TEXT("class"), UStateTreeState::StaticClass()->GetPathName());
		}

		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (InOutState->TryGetArrayField(TEXT("children"), Children) && Children != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Child : *Children)
			{
				const TSharedPtr<FJsonObject>* ChildObject = nullptr;
				if (Child.IsValid() && Child->TryGetObject(ChildObject))
				{
					InjectStateClass(*ChildObject);
				}
			}
		}
	}

	/** 造一份最小配置文档：asset 指向必定不存在的路径——dry-run 不碰盘。 */
	static bool ParseDocument(
		const FString& InRootBody,
		FForgeDocument& OutDocument,
		// 用 StateTreeModule 的 schema 基类路径：它 abstract 不能实例化，但 dry-run 只做类解析，
		// 不碰实例化——这样测试不依赖任何具体 schema 插件。
		const FString& InSchema = TEXT("/Script/StateTreeModule.StateTreeSchema"))
	{
		const FString Text = FString::Printf(TEXT(R"({
			"kind": "state-tree",
			"schemaVersion": 1,
			"revision": 1,
			"asset": "/Game/Test/AI/ST_ForgeProbe",
			"schema": "%s",
			"root": %s
		})"), *InSchema, *InRootBody);

		TSharedPtr<FJsonObject> Json;
		FString Error;
		if (!ForgeJsonText::ParseObject(Text, Json, Error))
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* RootObject = nullptr;
		if (Json->TryGetObjectField(TEXT("root"), RootObject))
		{
			InjectStateClass(*RootObject);
		}

		TArray<FForgeDiagnostic> Diagnostics;
		return ForgeDoc::Parse(Json, TEXT("test.json"), OutDocument, Diagnostics);
	}

	static bool HasDiagnosticAt(const TArray<FForgeDiagnostic>& InDiagnostics, const FString& InFieldPath, EForgeSeverity InSeverity)
	{
		for (const FForgeDiagnostic& Diagnostic : InDiagnostics)
		{
			if (Diagnostic.FieldPath == InFieldPath && Diagnostic.Severity == InSeverity)
			{
				return true;
			}
		}
		return false;
	}

	static bool HasDiagnosticAt(const FForgeReport& InReport, const FString& InFieldPath, EForgeSeverity InSeverity)
	{
		return HasDiagnosticAt(InReport.Diagnostics, InFieldPath, InSeverity);
	}

	/** 内存态状态树：不建包、不写盘，专供只读 API（引用解析 / 导出快照）的用例。 */
	struct FScratchTree
	{
		UStateTree* Tree = nullptr;
		UStateTreeEditorData* EditorData = nullptr;
		UStateTreeState* Root = nullptr;
	};

	static FScratchTree MakeScratchTree(const FName InRootName = TEXT("Root"))
	{
		FScratchTree Result;
		Result.Tree = NewObject<UStateTree>(GetTransientPackage(), NAME_None, RF_Transient);
		Result.EditorData = NewObject<UStateTreeEditorData>(Result.Tree, NAME_None, RF_Transient);
		Result.Tree->EditorData = Result.EditorData;

		// 刻意不设 Schema：树参数 / 状态参数 / 节点条目与具体 schema 无关，
		// 而任何具体 schema 都牵扯一个插件——测试不该把它绑进项目的插件集合。
		Result.Root = NewObject<UStateTreeState>(Result.EditorData, NAME_None, RF_Transient);
		Result.Root->Name = InRootName;
		Result.EditorData->SubTrees.Add(Result.Root);
		return Result;
	}

	static UStateTreeState& AddChildState(UStateTreeState& InParent, const FName InName)
	{
		UStateTreeState* Child = NewObject<UStateTreeState>(&InParent, NAME_None, RF_Transient);
		Child->Name = InName;
		Child->Parent = &InParent;
		InParent.Children.Add(Child);
		return *Child;
	}

	static FStateTreeEditorNode& AddTask(UStateTreeState& InState, UScriptStruct* InStruct, const FName InName = NAME_None)
	{
		FStateTreeEditorNode& Node = InState.Tasks.AddDefaulted_GetRef();
		Node.InitializeAs(&InState, InStruct);
		if (!InName.IsNone())
		{
			Node.SetNodeName(InName);
		}
		return Node;
	}

	static const TSharedPtr<FJsonObject>* GetObjectArrayEntry(const TSharedPtr<FJsonObject>& InOwner, const TCHAR* InField, int32 InIndex)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!InOwner.IsValid() || !InOwner->TryGetArrayField(InField, Array) || Array == nullptr)
		{
			return nullptr;
		}
		if (!Array->IsValidIndex(InIndex))
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!(*Array)[InIndex]->TryGetObject(Object))
		{
			return nullptr;
		}
		return Object;
	}

	// ---------- 接入 ----------

	TEST_METHOD(类型已登记且归类先于_data_asset)
	{
		ASSERT_THAT(IsTrue(
			ForgeKindRegistry::GetSupportedKinds().Contains(StateTreeForge::Kind),
			TEXT("state-tree 必须出现在支持的 kind 名单里（窗口与校验都读它）")));

		ASSERT_THAT(AreEqual(
			FString(StateTreeForge::Kind),
			ForgeKindRegistry::ClassifyAssetClass(UStateTree::StaticClass()),
			TEXT("UStateTree 是 UDataAsset 的子类——归类必须先于 data-asset 判定，否则会被误判")));
	}

	// ---------- dry-run 校验 ----------

	TEST_METHOD(缺少_schema_被拒绝)
	{
		TSharedPtr<FJsonObject> Json;
		FString Error;
		ASSERT_THAT(IsTrue(ForgeJsonText::ParseObject(TEXT(R"({
			"kind": "state-tree", "schemaVersion": 1, "revision": 1,
			"asset": "/Game/Test/AI/ST_ForgeProbe",
			"root": { "name": "Root" }
		})"), Json, Error), TEXT("测试文档本身应能解析")));

		FForgeDocument Document;
		TArray<FForgeDiagnostic> Diagnostics;

		const TSharedPtr<FJsonObject>* RootObject = nullptr;
		if (Json->TryGetObjectField(TEXT("root"), RootObject))
		{
			InjectStateClass(*RootObject);
		}

		ASSERT_THAT(IsTrue(ForgeDoc::Parse(Json, TEXT("test.json"), Document, Diagnostics)));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(StateTreeForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("schema 决定 EditorData 类与 context 结构，导入不能缺")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("schema"), EForgeSeverity::Error)));
	}

	TEST_METHOD(找不到的_schema_类被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			TEXT(R"({ "name": "Root" })"),
			Document,
			TEXT("/Script/NoSuchModule.NoSuchSchema"))));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(StateTreeForge::ImportDocument(Document, /*bDryRun=*/true, Report)));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("schema"), EForgeSeverity::Error)));
	}

	TEST_METHOD(找不到的节点类型被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			TEXT(R"({ "name": "Root", "tasks": [ { "type": "/Script/StateTreeModule.NoSuchTask" } ] })"),
			Document)));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(StateTreeForge::ImportDocument(Document, /*bDryRun=*/true, Report)));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.tasks[0].type"), EForgeSeverity::Error)));
	}

	TEST_METHOD(状态名不能含斜杠)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(TEXT(R"({ "name": "A/B" })"), Document)));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(StateTreeForge::ImportDocument(Document, /*bDryRun=*/true, Report)));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.name"), EForgeSeverity::Error)));
	}

	TEST_METHOD(同级重名状态被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(TEXT(R"({
			"name": "Root",
			"children": [ { "name": "Combat" }, { "name": "Combat" } ]
		})"), Document)));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(StateTreeForge::ImportDocument(Document, /*bDryRun=*/true, Report)));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root/Combat.name"), EForgeSeverity::Error)));
	}

	TEST_METHOD(转移缺少_trigger_被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(TEXT(R"({
			"name": "Root",
			"transitions": [ { "to": "Succeeded" } ]
		})"), Document)));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(StateTreeForge::ImportDocument(Document, /*bDryRun=*/true, Report)));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.transitions[0].trigger"), EForgeSeverity::Error)));
	}

	TEST_METHOD(目标任务写非法枚举被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(TEXT(R"({
			"name": "Root",
			"transitions": [ { "trigger": "OnTick", "to": "Succeeded", "priority": "Urgent" } ]
		})"), Document)));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(StateTreeForge::ImportDocument(Document, /*bDryRun=*/true, Report)));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.transitions[0].priority"), EForgeSeverity::Error)));
	}

	// ---------- 节点类型解析 ----------

	TEST_METHOD(节点类型解析_结构体路径)
	{
		UScriptStruct* Struct = nullptr;
		UClass* Class = nullptr;
		FString Error;

		ASSERT_THAT(IsTrue(StateTreeCodec::ResolveNodeType(
			TEXT("/Script/StateTreeModule.StateTreeTaskBase"), Struct, Class, Error),
			TEXT("任务基类结构应能解析")));
		ASSERT_THAT(IsTrue(Struct == FStateTreeTaskBase::StaticStruct()));
		ASSERT_THAT(IsTrue(Class == nullptr));

		ASSERT_THAT(IsFalse(StateTreeCodec::ResolveNodeType(
			TEXT("/Script/StateTreeModule.NoSuchThing"), Struct, Class, Error)));
	}

	// ---------- 引用解析 ----------

	TEST_METHOD(引用解析_状态参数与命名节点与全局参数)
	{
		const FScratchTree Scratch = MakeScratchTree(TEXT("Main"));
		FStateTreeEditorNode& Task = AddTask(*Scratch.Root, FStateTreeTaskBase::StaticStruct(), TEXT("Wait"));

		FGuid ID;
		FString Property;
		FString Error;

		ASSERT_THAT(IsTrue(StateTreeCodec::ResolveStructReference(
			*Scratch.EditorData, TEXT("Main/Parameters"), ID, Property, Error),
			TEXT("状态参数应可解析")));
		ASSERT_THAT(IsTrue(Property.IsEmpty()));

		ASSERT_THAT(IsTrue(StateTreeCodec::ResolveStructReference(
			*Scratch.EditorData, TEXT("Main/Wait.bConsideredForCompletion"), ID, Property, Error),
			TEXT("命名节点应可解析")));
		ASSERT_THAT(AreEqual(FString(TEXT("bConsideredForCompletion")), Property));
		ASSERT_THAT(IsTrue(ID == Task.ID, TEXT("解析出的应是该节点的实例数据 ID")));

		ASSERT_THAT(IsTrue(StateTreeCodec::ResolveStructReference(
			*Scratch.EditorData, TEXT("Global/Parameters"), ID, Property, Error),
			TEXT("树参数应可解析（Global 命名空间）")));

		ASSERT_THAT(IsFalse(StateTreeCodec::ResolveStructReference(
			*Scratch.EditorData, TEXT("Main/Nope"), ID, Property, Error)));
	}

	TEST_METHOD(引用解析_同名节点报歧义)
	{
		const FScratchTree Scratch = MakeScratchTree();
		AddTask(*Scratch.Root, FStateTreeTaskBase::StaticStruct(), TEXT("Wait"));
		AddTask(*Scratch.Root, FStateTreeTaskBase::StaticStruct(), TEXT("Wait"));

		FGuid ID;
		FString Property;
		FString Error;
		ASSERT_THAT(IsFalse(StateTreeCodec::ResolveStructReference(
			*Scratch.EditorData, TEXT("Root/Wait"), ID, Property, Error),
			TEXT("同名节点的引用必须判歧义，不能猜")));
		ASSERT_THAT(IsTrue(Error.Contains(TEXT("歧义")), TEXT("错误信息要提示怎么消歧（给节点起唯一 name）")));
	}

	// ---------- 导出快照 ----------

	TEST_METHOD(导出快照_状态层级_任务与转移)
	{
		const FScratchTree Scratch = MakeScratchTree(TEXT("Main"));
		UStateTreeState& Idle = AddChildState(*Scratch.Root, TEXT("Idle"));
		AddTask(*Scratch.Root, FStateTreeTaskBase::StaticStruct(), TEXT("Wait"));
		Scratch.Root->AddTransition(EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::GotoState, &Idle);

		FForgeDocument Document;
		TArray<FForgeDiagnostic> Diagnostics;
		StateTreeCodec::ExportDocument(*Scratch.Tree, Document, Diagnostics);

		ASSERT_THAT(AreEqual(FString(TEXT("Main")), Document.Root.Name));
		ASSERT_THAT(AreEqual(1, Document.Root.Children.Num()));
		ASSERT_THAT(AreEqual(FString(TEXT("Idle")), Document.Root.Children[0].Name));

		const TSharedPtr<FJsonObject>* TaskJson = GetObjectArrayEntry(Document.Root.Extra, StateTreeCodec::Field::Tasks, 0);
		ASSERT_THAT(IsTrue(TaskJson != nullptr, TEXT("root 应导出 tasks 数组")));
		if (TaskJson != nullptr)
		{
			ASSERT_THAT(AreEqual(FString(TEXT("Wait")), (*TaskJson)->GetStringField(TEXT("name"))));
			ASSERT_THAT(AreEqual(
				FStateTreeTaskBase::StaticStruct()->GetPathName(),
				(*TaskJson)->GetStringField(TEXT("type")),
				TEXT("C++ 节点导出结构体全路径")));
		}

		const TSharedPtr<FJsonObject>* TransitionJson = GetObjectArrayEntry(Document.Root.Extra, TEXT("transitions"), 0);
		ASSERT_THAT(IsTrue(TransitionJson != nullptr, TEXT("root 应导出 transitions 数组")));
		if (TransitionJson != nullptr)
		{
			ASSERT_THAT(AreEqual(
				FString(TEXT("Main/Idle")),
				(*TransitionJson)->GetStringField(TEXT("to")),
				TEXT("GotoState 目标应导出为状态路径（按 ID 反查）")));
			ASSERT_THAT(AreEqual(FString(TEXT("OnStateCompleted")), (*TransitionJson)->GetStringField(TEXT("trigger"))));
		}
	}

	// ---------- 参数 ----------

	TEST_METHOD(参数导入_按配置重建并删除未写的)
	{
		FInstancedPropertyBag Bag;
		Bag.AddProperty(TEXT("Keep"), EPropertyBagPropertyType::Bool);
		Bag.AddProperty(TEXT("Drop"), EPropertyBagPropertyType::Int32);
		ASSERT_THAT(IsTrue(Bag.IsValid()));

		TSharedPtr<FJsonObject> Json;
		FString Error;
		ASSERT_THAT(IsTrue(ForgeJsonText::ParseObject(
			TEXT(R"({ "Keep": false, "Count": 3, "Speed": 2.5, "Label": "hi", "Id": { "type": "Name", "value": "Target" } })"),
			Json,
			Error)));

		TArray<FForgeDiagnostic> Diagnostics;
		StateTreeCodec::ImportPropertyBag(Bag, Json, Diagnostics, TEXT("parameters"));
		for (const FForgeDiagnostic& Diagnostic : Diagnostics)
		{
			ASSERT_THAT(IsFalse(
				Diagnostic.Severity == EForgeSeverity::Error,
				Diagnostic.FieldPath + TEXT(": ") + Diagnostic.Message));
		}

		ASSERT_THAT(IsTrue(Bag.FindPropertyDescByName(TEXT("Keep")) != nullptr));
		ASSERT_THAT(IsTrue(Bag.FindPropertyDescByName(TEXT("Drop")) == nullptr, TEXT("配置未写的参数必须删除")));
		ASSERT_THAT(IsTrue(Bag.FindPropertyDescByName(TEXT("Count")) != nullptr));
		ASSERT_THAT(IsTrue(Bag.FindPropertyDescByName(TEXT("Speed")) != nullptr));
		ASSERT_THAT(IsTrue(Bag.FindPropertyDescByName(TEXT("Label")) != nullptr));
		ASSERT_THAT(IsTrue(Bag.FindPropertyDescByName(TEXT("Id")) != nullptr));

		if (auto KeepValue = Bag.GetValueBool(TEXT("Keep")); KeepValue.IsValid())
		{
			ASSERT_THAT(IsFalse(KeepValue.GetValue()));
		}
		if (auto CountValue = Bag.GetValueInt32(TEXT("Count")); CountValue.IsValid())
		{
			ASSERT_THAT(AreEqual(3, CountValue.GetValue()));
		}
		if (auto IdValue = Bag.GetValueName(TEXT("Id")); IdValue.IsValid())
		{
			ASSERT_THAT(IsTrue(IdValue.GetValue() == FName(TEXT("Target"))));
		}
	}

	TEST_METHOD(参数导出_Name_用显式形式)
	{
		FInstancedPropertyBag Bag;
		Bag.AddProperty(TEXT("Radius"), EPropertyBagPropertyType::Float);
		Bag.AddProperty(TEXT("Id"), EPropertyBagPropertyType::Name);
		Bag.SetValueFloat(TEXT("Radius"), 500.0f);
		Bag.SetValueName(TEXT("Id"), TEXT("Target"));

		TSharedPtr<FJsonObject> Out;
		TArray<FForgeDiagnostic> Diagnostics;
		StateTreeCodec::ExportPropertyBag(Bag, Out, Diagnostics, TEXT("parameters"));

		ASSERT_THAT(IsTrue(Out.IsValid()));
		ASSERT_THAT(IsTrue(Out->HasField(TEXT("Radius"))));

		const TSharedPtr<FJsonObject>* IdJson = nullptr;
		ASSERT_THAT(IsTrue(Out->TryGetObjectField(TEXT("Id"), IdJson), TEXT("Name 必须是显式形式（裸字符串会被推断为 String，往返漂移）")));
		if (IdJson != nullptr)
		{
			ASSERT_THAT(AreEqual(FString(TEXT("Name")), (*IdJson)->GetStringField(TEXT("type"))));
			ASSERT_THAT(AreEqual(FString(TEXT("Target")), (*IdJson)->GetStringField(TEXT("value"))));
		}
	}

	TEST_METHOD(参数容器类型给出诊断而非静默丢失)
	{
		FInstancedPropertyBag Bag;
		Bag.AddContainerProperty(TEXT("List"), EPropertyBagContainerType::Array, EPropertyBagPropertyType::Int32);

		TSharedPtr<FJsonObject> Out;
		TArray<FForgeDiagnostic> Diagnostics;
		StateTreeCodec::ExportPropertyBag(Bag, Out, Diagnostics, TEXT("parameters"));

		ASSERT_THAT(IsTrue(
			HasDiagnosticAt(Diagnostics, TEXT("parameters.List"), EForgeSeverity::Warning),
			TEXT("容器参数本版本不支持——必须给诊断，不能静默丢")));
	}
};

#endif // WITH_AUTOMATION_WORKER
