// Copyright Epic Games, Inc. All Rights Reserved.

#include "Kinds/Actor/ActorBlueprintForge.h"
#include "Core/ForgeClassResolver.h"
#include "Core/ForgeDiff.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgePropertyCodec.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Components/SceneComponent.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace ActorBlueprintForge
{

namespace
{
	UBlueprint* FindBlueprint(const FString& InAssetPath)
	{
		if (InAssetPath.IsEmpty())
		{
			return nullptr;
		}

		return Cast<UBlueprint>(LoadObject<UObject>(nullptr, *InAssetPath));
	}

	/** 组件类：含 `/` 或 `.` 的按全路径加载，否则按引擎的 Engine 模块短名解析。 */
	UClass* ResolveComponentClass(const FString& InClassName, FString& OutError)
	{
		if (InClassName.IsEmpty())
		{
			OutError = TEXT("组件缺少 class");
			return nullptr;
		}

		UClass* Class = nullptr;
		if (InClassName.Contains(TEXT("/")) || InClassName.Contains(TEXT(".")))
		{
			Class = LoadClass<UActorComponent>(nullptr, *InClassName);
		}
		else
		{
			Class = LoadClass<UActorComponent>(nullptr, *(FString(TEXT("/Script/Engine.")) + InClassName));
		}

		if (Class == nullptr)
		{
			OutError = FString::Printf(
				TEXT("找不到组件类 `%s`；短名只按 /Script/Engine 解析，其它模块请写全路径"),
				*InClassName);
			return nullptr;
		}

		if (Class->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = FString::Printf(TEXT("`%s` 是抽象类，建不出组件实例"), *InClassName);
			return nullptr;
		}

		return Class;
	}

	UClass* ResolveParentClass(const FString& InParentPath, FString& OutError)
	{
		if (InParentPath.IsEmpty())
		{
			OutError = TEXT("缺少 parent：Actor 蓝图必须指明父类（组件树挂在父类的组件之上）");
			return nullptr;
		}

		return FForgeClassResolver::Resolve(InParentPath, AActor::StaticClass(), OutError);
	}

	/**
	 * 引擎自己管的节点（默认场景根）。
	 *
	 * `ValidateSceneRootNodes` 会在合适的时候自动创建一个 `DefaultSceneRoot` 并放进根节点集合，
	 * 它不属于作者写的组件：同步时要跳过它，否则「删掉配置里没有的组件」会把它删掉，
	 * 而删它会触发子节点提升那套逻辑（引擎日志里那句 failed to find a parent node 就是它）。
	 */
	bool IsEngineManagedNode(const USCS_Node* InNode)
	{
		return InNode != nullptr
			&& InNode->GetVariableName() == USceneComponent::GetDefaultSceneRootVariableName();
	}

	/** 删掉一个组件及其整棵子树（`RemoveNodeAndPromoteChildren` 会把子节点提上来，不是我们要的）。 */
	void RemoveComponentSubtree(USimpleConstructionScript* InSCS, USCS_Node* InNode)
	{
		if (InSCS == nullptr || InNode == nullptr)
		{
			return;
		}

		TArray<USCS_Node*> Children = InNode->GetChildNodes();
		for (USCS_Node* Child : Children)
		{
			RemoveComponentSubtree(InSCS, Child);
		}

		InSCS->RemoveNode(InNode, /*bValidateSceneRootNodes=*/false);
	}

	/** 把一个 SCS 节点读成配置节点（含子树）。 */
	void ReadComponentInto(const USCS_Node* InNode, FForgeNode& OutNode, TArray<FForgeDiagnostic>& OutDiagnostics, const FString& InNodePath)
	{
		OutNode.Name = InNode->GetVariableName().ToString();
		OutNode.ClassName = (InNode->ComponentClass != nullptr)
			? InNode->ComponentClass->GetPathName()
			: FString();

		ForgePropertyCodec::ExportDeltaProperties(
			InNode->ComponentTemplate,
			OutNode.Props,
			OutDiagnostics,
			FString::Printf(TEXT("%s.props"), *InNodePath));

		for (const USCS_Node* ChildNode : InNode->GetChildNodes())
		{
			if (ChildNode == nullptr)
			{
				continue;
			}

			FForgeNode ChildConfig;
			ReadComponentInto(
				ChildNode,
				ChildConfig,
				OutDiagnostics,
				FString::Printf(TEXT("%s/%s"), *InNodePath, *ChildNode->GetVariableName().ToString()));
			OutNode.Children.Add(MoveTemp(ChildConfig));
		}
	}

	/** 把 SCS 的根节点集合读成配置树的 children。 */
	void ReadRootComponents(const USimpleConstructionScript* InSCS, FForgeNode& OutRoot, TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		OutRoot.ClassName = ComponentRootClass;
		OutRoot.Name = TEXT("Components");

		if (InSCS == nullptr)
		{
			return;
		}

		for (const USCS_Node* RootNode : InSCS->GetRootNodes())
		{
			if (RootNode == nullptr || IsEngineManagedNode(RootNode))
			{
				continue;
			}

			FForgeNode ChildConfig;
			ReadComponentInto(
				RootNode,
				ChildConfig,
				OutDiagnostics,
				FString::Printf(TEXT("root/%s"), *RootNode->GetVariableName().ToString()));
			OutRoot.Children.Add(MoveTemp(ChildConfig));
		}
	}

	/**
	 * 把 Actor 自身的类默认读进 `root.props`（等价于蓝图编辑器 Class Defaults 面板里改过的值）。
	 *
	 * 基准必须是**父类** CDO：生成类 CDO 上的值就是"当前值"本身，拿它当基准等于与自己比较，
	 * 导出恒为空、配置永不收敛（object-blueprint 踩过同一个坑、同一处修法）。它与组件树共用
	 * `root` 节点：`root.props` 是 Actor 自己的属性，`root.children` 是组件树。
	 */
	void ReadClassDefaults(const UBlueprint* InBlueprint, FForgeNode& OutRoot, TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		if (InBlueprint == nullptr
			|| InBlueprint->ParentClass == nullptr
			|| InBlueprint->GeneratedClass == nullptr)
		{
			return;
		}

		ForgePropertyCodec::ExportStructDelta(
			InBlueprint->ParentClass,
			InBlueprint->GeneratedClass->GetDefaultObject(),
			InBlueprint->ParentClass->GetDefaultObject(),
			OutRoot.Props,
			OutDiagnostics,
			TEXT("root.props"));
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

	/** 把一棵配置子树同步到 SCS：按变量名匹配。返回是否有改动。 */
	bool SyncComponents(USimpleConstructionScript* InSCS, UBlueprint* InBlueprint, USCS_Node* InParentNode, const FForgeNode& InTarget, FForgeReport& OutReport, const FString& InNodePath)
	{
		bool bChanged = false;

		// 当前这一层的现有节点：顶层取自 RootNodes，其余取自父节点的 ChildNodes。
		TArray<USCS_Node*> CurrentNodes;
		if (InParentNode != nullptr)
		{
			for (USCS_Node* Child : InParentNode->GetChildNodes())
			{
				if (Child != nullptr)
				{
					CurrentNodes.Add(Child);
				}
			}
		}
		else
		{
			for (USCS_Node* RootNode : InSCS->GetRootNodes())
			{
				if (RootNode != nullptr && !IsEngineManagedNode(RootNode))
				{
					CurrentNodes.Add(RootNode);
				}
			}
		}

		TMap<FString, USCS_Node*> CurrentByName;
		for (USCS_Node* Node : CurrentNodes)
		{
			CurrentByName.Add(Node->GetVariableName().ToString(), Node);
		}

		TSet<FString> TargetNames;

		for (const FForgeNode& ChildConfig : InTarget.Children)
		{
			TargetNames.Add(ChildConfig.Name);
			const FString ChildPath = FString::Printf(TEXT("%s/%s"), *InNodePath, *ChildConfig.Name);

			FString ClassError;
			UClass* ComponentClass = ResolveComponentClass(ChildConfig.ClassName, ClassError);
			if (ComponentClass == nullptr)
			{
				OutReport.AddError(FString::Printf(TEXT("%s.class"), *ChildPath), ClassError);
				continue;
			}

			USCS_Node** Existing = CurrentByName.Find(ChildConfig.Name);
			USCS_Node* Node = (Existing != nullptr) ? *Existing : nullptr;

			// 类型变了没法就地改：删掉重建（子组件会跟着重建，这是换类型的必然代价）。
			if (Node != nullptr && Node->ComponentClass != ComponentClass)
			{
				RemoveComponentSubtree(InSCS, Node);
				Node = nullptr;
				bChanged = true;
			}

			if (Node == nullptr)
			{
				Node = InSCS->CreateNode(ComponentClass, FName(*ChildConfig.Name));
				if (Node == nullptr)
				{
					OutReport.AddError(ChildPath, FString::Printf(TEXT("创建组件 `%s` 失败"), *ChildConfig.Name));
					continue;
				}

				if (InParentNode != nullptr)
				{
					InParentNode->AddChildNode(Node);
				}
				else
				{
					InSCS->AddNode(Node);
				}

				bChanged = true;
			}

			if (Node->ComponentTemplate != nullptr)
			{
				ForgePropertyCodec::ResetToDefaults(Node->ComponentTemplate);
				ForgePropertyCodec::ImportProperties(
					Node->ComponentTemplate,
					ChildConfig.Props,
					OutReport.Diagnostics,
					FString::Printf(TEXT("%s.props"), *ChildPath));
			}

			bChanged |= SyncComponents(InSCS, InBlueprint, Node, ChildConfig, OutReport, ChildPath);
		}

		// 配置里没有的组件：删。与 UMG 一致——配置是唯一真源。
		for (USCS_Node* Node : CurrentNodes)
		{
			if (Node != nullptr && !TargetNames.Contains(Node->GetVariableName().ToString()))
			{
				RemoveComponentSubtree(InSCS, Node);
				bChanged = true;
			}
		}

		return bChanged;
	}

	/** 计划阶段的校验：组件类能不能解析、属性写不写得进去。 */
	void ValidateSubtree(const FForgeNode& InNode, const FString& InNodePath, FForgeReport& OutReport)
	{
		for (const FForgeNode& ChildConfig : InNode.Children)
		{
			const FString ChildPath = FString::Printf(TEXT("%s/%s"), *InNodePath, *ChildConfig.Name);

			FString ClassError;
			UClass* ComponentClass = ResolveComponentClass(ChildConfig.ClassName, ClassError);
			if (ComponentClass == nullptr)
			{
				OutReport.AddError(FString::Printf(TEXT("%s.class"), *ChildPath), ClassError);
				continue;
			}

			ForgePropertyCodec::ValidateProperties(
				ComponentClass,
				ChildConfig.Props,
				OutReport.Diagnostics,
				FString::Printf(TEXT("%s.props"), *ChildPath));

			ValidateSubtree(ChildConfig, ChildPath, OutReport);
		}
	}

	/** 归一化：把配置里显式写出的默认值删掉，与导出形态对齐。 */
	void PruneSubtree(FForgeNode& InNode)
	{
		for (FForgeNode& ChildConfig : InNode.Children)
		{
			FString ClassError;
			if (UClass* ComponentClass = ResolveComponentClass(ChildConfig.ClassName, ClassError))
			{
				ForgePropertyCodec::PruneDefaults(ComponentClass, ChildConfig.Props);
			}

			PruneSubtree(ChildConfig);
		}
	}
}

bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport)
{
	OutReport.AssetPath = InAssetPath;

	UBlueprint* Blueprint = FindBlueprint(InAssetPath);
	if (Blueprint == nullptr)
	{
		OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("找不到蓝图：%s"), *InAssetPath));
		return false;
	}

	if (Blueprint->ParentClass == nullptr || !Blueprint->ParentClass->IsChildOf(AActor::StaticClass()))
	{
		OutReport.AddError(
			TEXT("parent"),
			FString::Printf(
				TEXT("%s 的父类不是 AActor 的派生；非 Actor 蓝图（如动画蓝图）各有自己的 kind"),
				*InAssetPath));
		return false;
	}

	OutDocument.Kind = Kind;
	OutDocument.SchemaVersion = ForgeDoc::CurrentSchemaVersion;
	OutDocument.Asset = InAssetPath;
	OutDocument.Parent = Blueprint->ParentClass->GetPathName();

	ReadRootComponents(Blueprint->SimpleConstructionScript, OutDocument.Root, OutReport.Diagnostics);
	ReadClassDefaults(Blueprint, OutDocument.Root, OutReport.Diagnostics);

	OutReport.bSucceeded = true;
	return true;
}

bool ImportDocument(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport)
{
	OutReport.bDryRun = bDryRun;
	OutReport.SourcePath = InDocument.SourcePath;
	OutReport.AssetPath = InDocument.Asset;

	FString ParentError;
	UClass* ParentClass = ResolveParentClass(InDocument.Parent, ParentError);
	if (ParentClass == nullptr)
	{
		OutReport.AddError(TEXT("parent"), ParentError);
		return false;
	}

	ValidateSubtree(InDocument.Root, TEXT("root"), OutReport);
	ForgePropertyCodec::ValidateProperties(
		ParentClass, InDocument.Root.Props, OutReport.Diagnostics, TEXT("root.props"));
	PruneSubtree(InDocument.Root);
	ForgePropertyCodec::PruneDefaults(ParentClass, InDocument.Root.Props);

	UBlueprint* Blueprint = FindBlueprint(InDocument.Asset);
	bool bCreatedAsset = false;

	// 计划：按组件变量名匹配，与 UMG 按控件名匹配是同一套。
	FForgeNode CurrentRoot;
	if (Blueprint != nullptr)
	{
		ReadRootComponents(Blueprint->SimpleConstructionScript, CurrentRoot, OutReport.Diagnostics);
		ReadClassDefaults(Blueprint, CurrentRoot, OutReport.Diagnostics);
	}
	else
	{
		CurrentRoot.ClassName = ComponentRootClass;
		CurrentRoot.Name = TEXT("Components");
	}

	ForgeDiff::ComputeRootPlan(InDocument.Root, CurrentRoot, OutReport.Actions);
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

	if (Blueprint == nullptr)
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
				TEXT("资产路径 `%s` 不合法，应形如 /Game/Blueprint/Pawn/BP_Xxx"), *InDocument.Asset));
			return false;
		}

		Blueprint = FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());

		if (Blueprint == nullptr)
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("创建蓝图失败：%s"), *InDocument.Asset));
			return false;
		}

		FAssetRegistryModule::AssetCreated(Blueprint);
		Package->MarkPackageDirty();

		// 新建的资产即使一份属性都不改也要落盘——"配置为空"不等于"不用建资产"。
		bCreatedAsset = true;
	}
	else if (Blueprint->ParentClass != ParentClass)
	{
		// 换父类会让已有组件失去附着点，也可能与父类的组件重名，不能悄悄做。
		OutReport.AddError(
			TEXT("parent"),
			FString::Printf(
				TEXT("现有蓝图的父类是 `%s`，与配置里的 `%s` 不一致；换父类请人工处理"),
				*Blueprint->ParentClass->GetPathName(),
				*InDocument.Parent));
		return false;
	}

	if (OutReport.Actions.Num() == 0 && !bCreatedAsset)
	{
		OutReport.bSucceeded = true;
		return true;
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (SCS == nullptr)
	{
		OutReport.AddError(TEXT("asset"), TEXT("蓝图没有 SimpleConstructionScript，无法挂组件"));
		return false;
	}

	SyncComponents(SCS, Blueprint, /*InParentNode=*/nullptr, InDocument.Root, OutReport, TEXT("root"));

	if (OutReport.HasErrors())
	{
		return false;
	}

	// root.props = Actor 自身的类默认：与 object-blueprint 同基准（父类 CDO）——先重置到父类默认，
	// 再写配置声明的值。配置是唯一真源，配置里没写的手改值也会被清掉。
	UObject* ClassDefaultObject = (Blueprint->GeneratedClass != nullptr)
		? Blueprint->GeneratedClass->GetDefaultObject()
		: nullptr;
	if (ClassDefaultObject == nullptr)
	{
		OutReport.AddError(TEXT("root.props"), TEXT("蓝图没有生成类，无法写类默认属性（先编译蓝图再试）"));
		return false;
	}

	ForgePropertyCodec::ResetStructToDefaults(ParentClass, ClassDefaultObject);
	ForgePropertyCodec::ImportStructProperties(
		ParentClass,
		ClassDefaultObject,
		InDocument.Root.Props,
		OutReport.Diagnostics,
		TEXT("root.props"));

	if (OutReport.HasErrors())
	{
		return false;
	}

	// 组件树 / 类默认变了要通知蓝图重建，否则生成类里不会带上这些改动。
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	FString SaveError;
	if (!SaveAssetPackage(Blueprint, SaveError))
	{
		OutReport.AddError(TEXT("asset"), SaveError);
		return false;
	}

	OutReport.bSucceeded = true;
	return true;
}

} // namespace ActorBlueprintForge
