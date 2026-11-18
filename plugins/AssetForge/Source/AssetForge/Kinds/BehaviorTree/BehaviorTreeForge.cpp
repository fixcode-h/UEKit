// Copyright Epic Games, Inc. All Rights Reserved.

#include "Kinds/BehaviorTree/BehaviorTreeForge.h"
#include "Core/ForgeDiff.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgePropertyCodec.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace BehaviorTreeForge
{

namespace
{
	const TCHAR* const FieldBlackboard = TEXT("blackboard");
	const TCHAR* const FieldDecorators = TEXT("decorators");
	const TCHAR* const FieldServices = TEXT("services");
	const TCHAR* const FieldNodeName = TEXT("NodeName");

	UBehaviorTree* FindBehaviorTree(const FString& InAssetPath)
	{
		if (InAssetPath.IsEmpty())
		{
			return nullptr;
		}

		return Cast<UBehaviorTree>(LoadObject<UObject>(nullptr, *InAssetPath));
	}

	/** 节点类：含 `/` 或 `.` 的按全路径加载，否则按 AIModule 的短名解析。 */
	UClass* ResolveNodeClass(const FString& InClassName, FString& OutError)
	{
		if (InClassName.IsEmpty())
		{
			OutError = TEXT("节点缺少 class");
			return nullptr;
		}

		UClass* Class = nullptr;
		if (InClassName.Contains(TEXT("/")) || InClassName.Contains(TEXT(".")))
		{
			Class = LoadClass<UBTNode>(nullptr, *InClassName);
		}
		else
		{
			Class = LoadClass<UBTNode>(nullptr, *(FString(TEXT("/Script/AIModule.")) + InClassName));
		}

		if (Class == nullptr)
		{
			OutError = FString::Printf(
				TEXT("找不到行为树节点类 `%s`；短名只按 /Script/AIModule 解析，项目自己的节点请写全路径"),
				*InClassName);
			return nullptr;
		}

		if (Class->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = FString::Printf(TEXT("`%s` 是抽象类，建不出节点"), *InClassName);
			return nullptr;
		}

		return Class;
	}

	/** 把一个节点（含子树、装饰器、服务）读成配置节点。 */
	void ReadNodeInto(const UBTNode* InNode, FForgeNode& OutNode, TArray<FForgeDiagnostic>& OutDiagnostics, const FString& InNodePath);

	/** 把一组附加节点（装饰器 / 服务）读成 JSON 数组。模板参数是因为两者的 TObjectPtr 类型不同。 */
	template <typename TNodeType>
	void ReadAttachedNodes(const TArray<TObjectPtr<TNodeType>>& InNodes, TArray<TSharedPtr<FJsonValue>>& OutArray, TArray<FForgeDiagnostic>& OutDiagnostics, const FString& InNodePath)
	{
		for (const TObjectPtr<TNodeType>& NodePtr : InNodes)
		{
			const UBTNode* Node = NodePtr.Get();
			if (Node == nullptr)
			{
				continue;
			}

			const TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
			NodeJson->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());

			TSharedPtr<FJsonObject> Props;
			ForgePropertyCodec::ExportDeltaProperties(Node, Props, OutDiagnostics, InNodePath);
			if (Props.IsValid())
			{
				Props->RemoveField(FieldNodeName);
				if (Props->Values.Num() > 0)
				{
					NodeJson->SetObjectField(TEXT("props"), Props);
				}
			}

			OutArray.Add(MakeShared<FJsonValueObject>(NodeJson));
		}
	}

	void ReadNodeInto(const UBTNode* InNode, FForgeNode& OutNode, TArray<FForgeDiagnostic>& OutDiagnostics, const FString& InNodePath)
	{
		if (InNode == nullptr)
		{
			return;
		}

		OutNode.Name = InNode->GetNodeName();
		OutNode.ClassName = InNode->GetClass()->GetPathName();

		ForgePropertyCodec::ExportDeltaProperties(InNode, OutNode.Props, OutDiagnostics, InNodePath);

		// NodeName 是匹配键，已经写在 name 里了，不该在 props 里重复出现。
		if (OutNode.Props.IsValid())
		{
			OutNode.Props->RemoveField(FieldNodeName);
		}

		const UBTCompositeNode* Composite = Cast<UBTCompositeNode>(InNode);
		if (Composite == nullptr)
		{
			return;
		}

		const TSharedPtr<FJsonObject> Extra = MakeShared<FJsonObject>();

		if (Composite->Services.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Services;
			ReadAttachedNodes(Composite->Services, Services, OutDiagnostics, InNodePath);
			Extra->SetArrayField(FieldServices, Services);
		}

		for (const FBTCompositeChild& Child : Composite->Children)
		{
			const UBTNode* ChildNode = (Child.ChildComposite != nullptr)
				? static_cast<const UBTNode*>(Child.ChildComposite)
				: static_cast<const UBTNode*>(Child.ChildTask);

			if (ChildNode == nullptr)
			{
				continue;
			}

			FForgeNode ChildConfig;
			ReadNodeInto(
				ChildNode,
				ChildConfig,
				OutDiagnostics,
				FString::Printf(TEXT("%s/%s"), *InNodePath, *ChildNode->GetNodeName()));

			// 装饰器挂在边上，但配置里写在子节点自己身上——两边语义上是同一件事。
			if (Child.Decorators.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Decorators;
				ReadAttachedNodes(Child.Decorators, Decorators, OutDiagnostics, InNodePath);

				if (!ChildConfig.Extra.IsValid())
				{
					ChildConfig.Extra = MakeShared<FJsonObject>();
				}
				ChildConfig.Extra->SetArrayField(FieldDecorators, Decorators);
			}

			OutNode.Children.Add(MoveTemp(ChildConfig));
		}

		if (Extra->Values.Num() > 0)
		{
			OutNode.Extra = Extra;
		}
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

	// ---- 同步 ----

	/** 读一个 JSON 数组里的附加节点定义。 */
	void ReadAttachedDefinitions(const TSharedPtr<FJsonObject>& InExtra, const TCHAR* InField, TArray<TSharedPtr<FJsonObject>>& OutDefinitions)
	{
		if (!InExtra.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!InExtra->TryGetArrayField(InField, Array) || Array == nullptr)
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (Value.IsValid() && Value->TryGetObject(Object) && Object->IsValid())
			{
				OutDefinitions.Add(*Object);
			}
		}
	}

	/** 校验一组附加节点定义（装饰器 / 服务）的类——与 `RebuildAttachedNodes` 同一判据。 */
	void ValidateAttachedDefinitions(const TSharedPtr<FJsonObject>& InExtra, const TCHAR* InField, UClass* InBaseClass, const FString& InNodePath, FForgeReport& OutReport)
	{
		TArray<TSharedPtr<FJsonObject>> Definitions;
		ReadAttachedDefinitions(InExtra, InField, Definitions);

		for (const TSharedPtr<FJsonObject>& Definition : Definitions)
		{
			FString ClassName;
			Definition->TryGetStringField(TEXT("class"), ClassName);

			FString ClassError;
			UClass* NodeClass = ResolveNodeClass(ClassName, ClassError);
			if (NodeClass == nullptr || !NodeClass->IsChildOf(InBaseClass))
			{
				OutReport.AddError(
					FString::Printf(TEXT("%s.attached.class"), *InNodePath),
					FString::Printf(TEXT("`%s` 不是 %s 的派生类"), *ClassName, *InBaseClass->GetName()));
			}
		}
	}

	/** 按定义重建一组附加节点（装饰器或服务）——它们没有稳定名字，增量匹配不值当。 */
	template <typename TNodeType>
	void RebuildAttachedNodes(UBehaviorTree* InTree, TArray<TObjectPtr<TNodeType>>& InOutNodes, const TArray<TSharedPtr<FJsonObject>>& InDefinitions, FForgeReport& OutReport, const FString& InNodePath)
	{
		InOutNodes.Empty();

		for (const TSharedPtr<FJsonObject>& Definition : InDefinitions)
		{
			FString ClassName;
			Definition->TryGetStringField(TEXT("class"), ClassName);

			FString ClassError;
			UClass* NodeClass = ResolveNodeClass(ClassName, ClassError);
			if (NodeClass == nullptr || !NodeClass->IsChildOf(TNodeType::StaticClass()))
			{
				OutReport.AddError(
					FString::Printf(TEXT("%s.attached.class"), *InNodePath),
					FString::Printf(TEXT("`%s` 不是 %s 的派生类"), *ClassName, *TNodeType::StaticClass()->GetName()));
				continue;
			}

			TNodeType* Node = NewObject<TNodeType>(InTree, NodeClass);
			if (Node == nullptr)
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* Props = nullptr;
			if (Definition->TryGetObjectField(TEXT("props"), Props) && Props->IsValid())
			{
				ForgePropertyCodec::ImportProperties(Node, *Props, OutReport.Diagnostics, InNodePath);
			}

			InOutNodes.Add(Node);
		}
	}

	/** 把一个配置节点同步到一个已存在的行为树节点上（改属性 + 递归子节点）。 */
	void SyncNode(UBehaviorTree* InTree, UBTNode* InNode, const FForgeNode& InTarget, FForgeReport& OutReport, const FString& InNodePath)
	{
		InNode->NodeName = InTarget.Name;

		ForgePropertyCodec::ResetToDefaults(InNode);
		ForgePropertyCodec::ImportProperties(InNode, InTarget.Props, OutReport.Diagnostics, InNodePath);

		UBTCompositeNode* Composite = Cast<UBTCompositeNode>(InNode);
		if (Composite == nullptr)
		{
			if (InTarget.Children.Num() > 0)
			{
				OutReport.AddError(
					FString::Printf(TEXT("%s.children"), *InNodePath),
					FString::Printf(TEXT("`%s` 不是 composite，不能有子节点"), *InTarget.Name));
			}
			return;
		}

		// 服务挂在节点自己身上。
		TArray<TSharedPtr<FJsonObject>> ServiceDefinitions;
		ReadAttachedDefinitions(InTarget.Extra, FieldServices, ServiceDefinitions);
		RebuildAttachedNodes<UBTService>(InTree, Composite->Services, ServiceDefinitions, OutReport, InNodePath);

		// 子节点**按位置匹配**：行为树的节点没有稳定名字（`NodeName` 不落盘，
		// 同级的两个 Wait 是常态），位置就是它的身份。这也意味着在中间插一个节点，
		// 它后面的兄弟会被视为"类型变了"而重建——这是行为树本身的语义，不是偷懒。
		TArray<FBTCompositeChild> NewChildren;
		NewChildren.Reserve(InTarget.Children.Num());

		for (int32 Index = 0; Index < InTarget.Children.Num(); ++Index)
		{
			const FForgeNode& ChildTarget = InTarget.Children[Index];
			const FString ChildPath = FString::Printf(TEXT("%s/%s"), *InNodePath, *ChildTarget.Name);

			FString ClassError;
			UClass* NodeClass = ResolveNodeClass(ChildTarget.ClassName, ClassError);
			if (NodeClass == nullptr)
			{
				OutReport.AddError(FString::Printf(TEXT("%s.class"), *ChildPath), ClassError);
				continue;
			}

			UBTNode* ChildNode = nullptr;

			if (Composite->Children.IsValidIndex(Index))
			{
				FBTCompositeChild& Existing = Composite->Children[Index];
				UBTNode* ExistingNode = (Existing.ChildComposite != nullptr)
					? static_cast<UBTNode*>(Existing.ChildComposite)
					: static_cast<UBTNode*>(Existing.ChildTask);

				// 类型一致就复用（保住它的子树与已设属性），否则重建。
				if (ExistingNode != nullptr && ExistingNode->GetClass() == NodeClass)
				{
					ChildNode = ExistingNode;
				}
			}

			if (ChildNode == nullptr)
			{
				ChildNode = NewObject<UBTNode>(InTree, NodeClass);
				if (ChildNode == nullptr)
				{
					OutReport.AddError(ChildPath, FString::Printf(TEXT("创建节点 `%s` 失败"), *ChildTarget.Name));
					continue;
				}
			}

			SyncNode(InTree, ChildNode, ChildTarget, OutReport, ChildPath);

			FBTCompositeChild NewChild;
			if (UBTCompositeNode* AsComposite = Cast<UBTCompositeNode>(ChildNode))
			{
				NewChild.ChildComposite = AsComposite;
			}
			else if (UBTTaskNode* AsTask = Cast<UBTTaskNode>(ChildNode))
			{
				NewChild.ChildTask = AsTask;
			}
			else
			{
				OutReport.AddError(
					ChildPath,
					FString::Printf(TEXT("`%s` 既不是 composite 也不是 task，不能当子节点"), *ChildTarget.Name));
				continue;
			}

			// 装饰器挂在边上——配置写在子节点身上。
			TArray<TSharedPtr<FJsonObject>> DecoratorDefinitions;
			ReadAttachedDefinitions(ChildTarget.Extra, FieldDecorators, DecoratorDefinitions);
			RebuildAttachedNodes<UBTDecorator>(InTree, NewChild.Decorators, DecoratorDefinitions, OutReport, ChildPath);

			NewChildren.Add(MoveTemp(NewChild));
		}

		Composite->Children = MoveTemp(NewChildren);
	}

	/**
	 * 计划阶段的子树校验——与真跑（`SyncNode` / `RebuildAttachedNodes`）**同一套判据**：
	 * 类必须能解析、非 composite 不能有子节点、装饰器/服务的类必须对。
	 *
	 * 少一条判据，dry-run 就会给出「没问题」的假计划、真跑才报错——两者必须是同一张门。
	 */
	void ValidateSubtree(const FForgeNode& InNode, const FString& InNodePath, FForgeReport& OutReport)
	{
		FString ClassError;
		UClass* NodeClass = ResolveNodeClass(InNode.ClassName, ClassError);
		if (NodeClass == nullptr)
		{
			OutReport.AddError(FString::Printf(TEXT("%s.class"), *InNodePath), ClassError);
			return;
		}

		if (InNode.Children.Num() > 0 && !NodeClass->IsChildOf(UBTCompositeNode::StaticClass()))
		{
			OutReport.AddError(
				FString::Printf(TEXT("%s.children"), *InNodePath),
				FString::Printf(TEXT("`%s` 不是 composite，不能有子节点"), *InNode.Name));
		}

		ForgePropertyCodec::ValidateProperties(NodeClass, InNode.Props, OutReport.Diagnostics, InNodePath);

		ValidateAttachedDefinitions(InNode.Extra, FieldDecorators, UBTDecorator::StaticClass(), InNodePath, OutReport);
		ValidateAttachedDefinitions(InNode.Extra, FieldServices, UBTService::StaticClass(), InNodePath, OutReport);

		for (const FForgeNode& Child : InNode.Children)
		{
			ValidateSubtree(Child, FString::Printf(TEXT("%s/%s"), *InNodePath, *Child.Name), OutReport);
		}
	}

	void PruneSubtree(FForgeNode& InNode)
	{
		FString ClassError;
		if (UClass* NodeClass = ResolveNodeClass(InNode.ClassName, ClassError))
		{
			ForgePropertyCodec::PruneDefaults(NodeClass, InNode.Props);
		}

		for (FForgeNode& Child : InNode.Children)
		{
			PruneSubtree(Child);
		}
	}

	/**
	 * 把每个子节点的名字换成「原名@序号」。
	 *
	 * 行为树的节点没有稳定名字（`UBTNode::NodeName` 不落盘，同级两个 `Wait` 是常态），
	 * 身份是**位置**。而计划算法 `ComputePlan` 按名字匹配——名字重复时后一个会覆盖前一个，
	 * 于是「配置里第一个 Wait」被拿去和「资产里第二个 Wait」比，报出一个假的 update：
	 * 资产每次都被重写，却永远不收敛。两边都按位置改名后，按名匹配等价于按位置匹配，
	 * 与执行侧（`SyncNode` 递归）一致。
	 */
	void AssignPositionalNames(FForgeNode& InOutNode)
	{
		for (int32 Index = 0; Index < InOutNode.Children.Num(); ++Index)
		{
			FForgeNode& Child = InOutNode.Children[Index];
			Child.Name = FString::Printf(TEXT("%s@%d"), *Child.Name, Index);
			AssignPositionalNames(Child);
		}
	}
}

bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport)
{
	OutReport.AssetPath = InAssetPath;

	UBehaviorTree* Tree = FindBehaviorTree(InAssetPath);
	if (Tree == nullptr)
	{
		OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("找不到行为树：%s"), *InAssetPath));
		return false;
	}

	if (Tree->RootNode == nullptr)
	{
		OutReport.AddError(TEXT("root"), FString::Printf(TEXT("%s 没有根节点，无法导出"), *InAssetPath));
		return false;
	}

	OutDocument.Kind = Kind;
	OutDocument.SchemaVersion = ForgeDoc::CurrentSchemaVersion;
	OutDocument.Asset = InAssetPath;
	OutDocument.Parent = BehaviorTreeClassPath;

	if (Tree->BlackboardAsset != nullptr)
	{
		OutDocument.Extra = MakeShared<FJsonObject>();
		OutDocument.Extra->SetStringField(FieldBlackboard, Tree->BlackboardAsset->GetPathName());
	}

	ReadNodeInto(Tree->RootNode, OutDocument.Root, OutReport.Diagnostics, TEXT("root"));

	OutReport.bSucceeded = true;
	return true;
}

bool ImportDocument(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport)
{
	OutReport.bDryRun = bDryRun;
	OutReport.SourcePath = InDocument.SourcePath;
	OutReport.AssetPath = InDocument.Asset;

	FString RootClassError;
	UClass* RootClass = ResolveNodeClass(InDocument.Root.ClassName, RootClassError);
	if (RootClass == nullptr || !RootClass->IsChildOf(UBTCompositeNode::StaticClass()))
	{
		OutReport.AddError(
			TEXT("root.class"),
			FString::Printf(
				TEXT("`%s` 不能当行为树根节点：根必须是 composite（如 BTComposite_Selector）"),
				*InDocument.Root.ClassName));
		return false;
	}

	ValidateSubtree(InDocument.Root, TEXT("root"), OutReport);
	PruneSubtree(InDocument.Root);

	UBehaviorTree* Tree = FindBehaviorTree(InDocument.Asset);

	// 计划：按节点名匹配，与其它 kind 一致。
	FForgeNode CurrentRoot;
	if (Tree != nullptr && Tree->RootNode != nullptr)
	{
		ReadNodeInto(Tree->RootNode, CurrentRoot, OutReport.Diagnostics, TEXT("root"));
	}
	else
	{
		CurrentRoot.ClassName = InDocument.Root.ClassName;
		CurrentRoot.Name = InDocument.Root.Name;
	}

	// 计划用「按位置命名」的副本：名字在这类资产里会重复，而计划算法按名匹配（见 AssignPositionalNames）。
	FForgeNode TargetForPlan = InDocument.Root;
	FForgeNode CurrentForPlan = CurrentRoot;
	AssignPositionalNames(TargetForPlan);
	AssignPositionalNames(CurrentForPlan);

	ForgeDiff::ComputeRootPlan(TargetForPlan, CurrentForPlan, OutReport.Actions);
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

	// 没有动作就不要写盘：无意义的保存会改时间戳、触发下游重导，也让「哪里变了」这件事失真。
	if (OutReport.Actions.Num() == 0)
	{
		OutReport.bSucceeded = true;
		return true;
	}

	if (Tree == nullptr)
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
			OutReport.AddError(TEXT("asset"), FString::Printf(
				TEXT("资产路径 `%s` 不合法，应形如 /Game/AI/BT_Xxx"), *InDocument.Asset));
			return false;
		}

		Tree = NewObject<UBehaviorTree>(Package, FName(*AssetName), RF_Public | RF_Standalone);
		if (Tree == nullptr)
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("创建行为树失败：%s"), *InDocument.Asset));
			return false;
		}

		FAssetRegistryModule::AssetCreated(Tree);
		Package->MarkPackageDirty();
	}

	// 黑板：换它会让所有黑板节点失效，与现有资产不一致时报错而不是悄悄改。
	FString BlackboardPath;
	if (InDocument.Extra.IsValid())
	{
		InDocument.Extra->TryGetStringField(FieldBlackboard, BlackboardPath);
	}

	if (!BlackboardPath.IsEmpty())
	{
		UBlackboardData* Blackboard = LoadObject<UBlackboardData>(nullptr, *BlackboardPath);
		if (Blackboard == nullptr)
		{
			OutReport.AddError(
				FieldBlackboard,
				FString::Printf(TEXT("找不到黑板资产 `%s`"), *BlackboardPath));
			return false;
		}

		Tree->BlackboardAsset = Blackboard;
	}

	if (Tree->RootNode == nullptr || Tree->RootNode->GetClass() != RootClass)
	{
		Tree->RootNode = NewObject<UBTCompositeNode>(Tree, RootClass);
	}

	if (Tree->RootNode == nullptr)
	{
		OutReport.AddError(TEXT("root"), TEXT("创建根节点失败"));
		return false;
	}

	SyncNode(Tree, Tree->RootNode, InDocument.Root, OutReport, TEXT("root"));

	if (OutReport.HasErrors())
	{
		return false;
	}

	// 丢掉编辑器图这份快照。
	//
	// BT 的编辑器图（`BTGraph`）是**运行时树的一份快照**，而打开资产时引擎走的是
	// `OnLoaded` → `UpdateAsset` → `CreateBTFromGraph`：**从图重建运行时树**。
	// 我们只改了树、从不碰图，那份旧图就会在用户打开资产的那一刻把这次导入悄悄回滚。
	//
	// 置空之后编辑器按「新图」处理（`FBehaviorTreeEditor::CreateGraph` 的 bNewGraph 分支），
	// 从树重新铺一遍：`SpawnMissingNodes` 会给每个节点按父节点算坐标
	// （BehaviorTreeGraph.cpp:851，`ParentPosX + ChildIdx * 400`），于是既不丢改动，
	// 也不会几个节点叠在同一个位置上。
	//
	// 代价是手工在图里加的注释框会随之消失——图只是树的一个视图，配置才是真源。
#if WITH_EDITORONLY_DATA
	Tree->BTGraph = nullptr;
#endif

	FString SaveError;
	if (!SaveAssetPackage(Tree, SaveError))
	{
		OutReport.AddError(TEXT("asset"), SaveError);
		return false;
	}

	OutReport.bSucceeded = true;
	return true;
}

} // namespace BehaviorTreeForge
