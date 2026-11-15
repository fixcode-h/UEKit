// Copyright Epic Games, Inc. All Rights Reserved.

#include "Kinds/Umg/UmgForge.h"
#include "Kinds/Umg/UmgAnimationCodec.h"
#include "Kinds/Umg/UmgWidgetCodec.h"
#include "Core/ForgeClassResolver.h"
#include "Core/ForgeDiff.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgePropertyCodec.h"

#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "WidgetBlueprint.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace UmgForge
{

namespace
{
	// SyncWidget 管「一个控件的属性 + 它的子节点」，SyncChildren 管「面板的子控件集合」。
	// 两者互相递归（面板要同步子控件，子控件又要同步自己的子树），所以先声明后定义。
	bool SyncWidget(
		UWidget* InWidget,
		const FForgeNode& InTarget,
		UWidgetTree* InTree,
		FForgeReport& OutReport,
		const FString& InNodePath);

	bool SyncChildren(
		UPanelWidget* InPanel,
		UWidgetTree* InTree,
		const FForgeNode& InTarget,
		FForgeReport& OutReport,
		const FString& InNodePath);

	FString JoinNodePath(const FString& InParentPath, const FString& InName)
	{
		return InParentPath.IsEmpty() ? InName : FString::Printf(TEXT("%s/%s"), *InParentPath, *InName);
	}

	UWidgetBlueprint* GetOwningBlueprint(const UWidgetTree* InTree)
	{
		return (InTree != nullptr) ? Cast<UWidgetBlueprint>(InTree->GetOuter()) : nullptr;
	}

	/**
	 * 控件增删必须同步蓝图的「变量名 → GUID」映射。
	 *
	 * 不做的后果是编译时的 ensure：删掉的控件在表里留着 GUID、新增的控件查不到 GUID——
	 * 引擎的控件工厂在创建根控件后也走同一步（`WidgetBlueprintFactory.cpp:198`）。
	 */
	void NotifyVariableAdded(UWidgetTree* InTree, const UWidget* InWidget)
	{
		if (UWidgetBlueprint* OwningBlueprint = GetOwningBlueprint(InTree))
		{
			if (InWidget != nullptr)
			{
				OwningBlueprint->OnVariableAdded(InWidget->GetFName());
			}
		}
	}

	void NotifyVariableRemoved(UWidgetTree* InTree, const UWidget* InWidget)
	{
		if (UWidgetBlueprint* OwningBlueprint = GetOwningBlueprint(InTree))
		{
			if (InWidget != nullptr)
			{
				OwningBlueprint->OnVariableRemoved(InWidget->GetFName());
			}
		}
	}

	/**
	 * 编译前把「变量名 → GUID」表与控件树对齐——把 UMG 编译器的两条 ensure 变成不可能事件。
	 *
	 * 引擎在编译时校验这张表：树里有而表里没有（`Widget was added but did not get a GUID`）、
	 * 表里有而树里没有（`Variable was deleted but still has a GUID`），两条都是 `ensureAlwaysMsgf`
	 * （`WidgetBlueprintCompiler.cpp:781/815`）。本项目的 ensure 配置会把它们升级成崩溃——
	 * 编辑器直接退出，所以**不能只指望"每条增删路径都记得登记"**：漏掉一处，用户遇到的就是崩溃
	 * 而不是一条报告。这里在编译前统一补齐/清理，使两条 ensure 永远没有触发条件
	 * （引擎自己的修复分支也只是直接改这张表）。
	 */
	void AlignVariableGuidMap(UWidgetBlueprint& InBlueprint)
	{
		if (InBlueprint.WidgetTree == nullptr)
		{
			return;
		}

		TSet<FName> LiveNames;
		InBlueprint.ForEachSourceWidget([&InBlueprint, &LiveNames](UWidget* Widget)
		{
			if (Widget == nullptr)
			{
				return;
			}

			const FName WidgetName = Widget->GetFName();
			LiveNames.Add(WidgetName);

			if (!InBlueprint.WidgetVariableNameToGuidMap.Contains(WidgetName))
			{
				// 确定性 GUID：与引擎首次生成时的算法一致，反复重编译不会漂移。
				InBlueprint.WidgetVariableNameToGuidMap.Emplace(
					WidgetName, FGuid::NewDeterministicGuid(Widget->GetPathName()));
			}
		});

		for (UWidgetAnimation* Animation : InBlueprint.Animations)
		{
			if (Animation == nullptr)
			{
				continue;
			}

			const FName AnimationName = Animation->GetFName();
			LiveNames.Add(AnimationName);

			if (!InBlueprint.WidgetVariableNameToGuidMap.Contains(AnimationName))
			{
				InBlueprint.WidgetVariableNameToGuidMap.Emplace(
					AnimationName, FGuid::NewDeterministicGuid(Animation->GetPathName()));
			}
		}

		for (auto It = InBlueprint.WidgetVariableNameToGuidMap.CreateIterator(); It; ++It)
		{
			if (!LiveNames.Contains(It.Key()))
			{
				It.RemoveCurrent();
			}
		}
	}

	/**
	 * 把旧控件（连同整棵子树）从控件树里摘干净、改名挪进瞬态包。
	 *
	 * 换控件类型或换根控件之前**必须**先做这一步：引擎不允许"用不同类替换同名对象"，
	 * 同名构造会直接 fatal（`Cannot replace existing object of a different class`）。
	 * 子树一起摘是因为子控件的名字同样会挡住新节点下的同名子控件。
	 * 改名挪进瞬态包是引擎自己的做法（见 `FWidgetBlueprintEditorUtils::DeleteWidgets`）。
	 */
	void DetachWidgetSubtree(UWidgetTree* InTree, UWidget* InWidget)
	{
		if (InWidget == nullptr)
		{
			return;
		}

		if (UPanelWidget* Panel = Cast<UPanelWidget>(InWidget))
		{
			for (int32 Index = Panel->GetChildrenCount() - 1; Index >= 0; --Index)
			{
				DetachWidgetSubtree(InTree, Panel->GetChildAt(Index));
			}
		}

		NotifyVariableRemoved(InTree, InWidget);
		InTree->RemoveWidget(InWidget);
		InWidget->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
	}

	UWidgetBlueprint* FindBlueprint(const FString& InAssetPath)
	{
		if (InAssetPath.IsEmpty())
		{
			return nullptr;
		}

		return Cast<UWidgetBlueprint>(StaticLoadObject(UWidgetBlueprint::StaticClass(), nullptr, *InAssetPath));
	}

	UClass* ResolveParentClass(const FString& InParentPath, FString& OutError)
	{
		if (InParentPath.IsEmpty())
		{
			// 省略 parent：WBP 的默认父类。
			return UUserWidget::StaticClass();
		}

		// 统一解析器：除常规加载外还认识 UnrealSharp 的 C# 类（`/Script/UnrealSharp.X_C`），
		// 解析失败给出可操作的中文诊断，绝不在这里抛异常。
		return FForgeClassResolver::Resolve(InParentPath, UUserWidget::StaticClass(), OutError);
	}

	UWidgetBlueprint* CreateBlueprint(const FForgeDocument& InDocument, UClass* InParentClass, FString& OutError)
	{
		FString PackagePath;
		FString AssetName;
		InDocument.Asset.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (AssetName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("资产路径 `%s` 不合法，应形如 /Game/UI/HUD/WBP_Xxx"), *InDocument.Asset);
			return nullptr;
		}

		UPackage* Package = CreatePackage(*InDocument.Asset);
		if (Package == nullptr)
		{
			OutError = FString::Printf(TEXT("无法创建包：%s"), *InDocument.Asset);
			return nullptr;
		}

		// 走 FKismetEditorUtilities 而不是 UWidgetBlueprintFactory：后者的 ConfigureProperties
		// 会弹类选择对话框，纯代码路径必须绕开它（引擎的工厂实现内部也是走这一条）。
		UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			InParentClass,
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UWidgetBlueprint::StaticClass(),
			UWidgetBlueprintGeneratedClass::StaticClass()));

		if (Blueprint == nullptr)
		{
			OutError = FString::Printf(TEXT("创建 UWidgetBlueprint 失败：%s"), *InDocument.Asset);
			return nullptr;
		}

		FAssetRegistryModule::AssetCreated(Blueprint);
		Package->MarkPackageDirty();
		return Blueprint;
	}

	void MarkSubtreeCreate(const FForgeNode& InNode, const FString& InNodePath, TArray<FForgePlanEntry>& OutPlan)
	{
		OutPlan.Add({ EForgeAction::Create, InNodePath, InNode.Name, FString() });

		for (const FForgeNode& Child : InNode.Children)
		{
			MarkSubtreeCreate(Child, JoinNodePath(InNodePath, Child.Name), OutPlan);
		}
	}

	/**
	 * 计划阶段的属性校验：在动资产之前，把每个节点上「写不进去」的字段全找出来。
	 *
	 * 为什么值得单独跑一遍：属性写入失败发生在同步过程中，那时资产可能刚被创建出来——
	 * 结果是包已建好却没能保存，留下一个半成品。校验前置就没有这个问题。
	 * 插槽的可用字段由**父控件**的槽类型决定，所以父类要往下传。
	 */
	void ValidateSubtree(
		FForgeNode& InNode,
		UClass* InParentWidgetClass,
		const FString& InNodePath,
		FForgeReport& OutReport)
	{
		FString ClassError;
		UClass* WidgetClass = UmgWidgetCodec::ResolveWidgetClass(InNode.ClassName, ClassError);
		if (WidgetClass == nullptr)
		{
			OutReport.AddError(JoinNodePath(InNodePath, TEXT("class")), ClassError);
			return;
		}

		// 抽象类建不出实例。引擎对此只在保存时说一句 warning 然后把这个控件置空，
		// 导入这边看起来却是「成功」——子界面写成 `UserWidget` 就会踩到这里
		// （UUserWidget 正是抽象的；子界面要写它自己的生成类）。
		if (WidgetClass->HasAnyClassFlags(CLASS_Abstract))
		{
			OutReport.AddError(
				JoinNodePath(InNodePath, TEXT("class")),
				FString::Printf(
					TEXT("`%s` 是抽象类，不能作为控件；引用子界面时要写它自己的生成类（如 /Game/.../WBP_X.WBP_X_C）"),
					*WidgetClass->GetName()));
			return;
		}

		ForgePropertyCodec::ValidateProperties(
			WidgetClass,
			InNode.Props,
			OutReport.Diagnostics,
			FString::Printf(TEXT("%s.props"), *InNodePath));

		// 归一化：把显式写出的默认值删掉，与导出方向（delta，默认值不写）对齐。
		// 放在校验之后——校验要看的是作者写了什么，归一化改的是拿去比较的形态。
		ForgePropertyCodec::PruneDefaults(WidgetClass, InNode.Props);

		if (InNode.Slot.IsValid() && InNode.Slot->Values.Num() > 0)
		{
			const UPanelWidget* ParentCDO = (InParentWidgetClass != nullptr)
				? Cast<UPanelWidget>(InParentWidgetClass->GetDefaultObject())
				: nullptr;

			if (ParentCDO != nullptr)
			{
				UClass* SlotClass = ParentCDO->GetSlotClass();
				ForgePropertyCodec::ValidateProperties(
					SlotClass,
					InNode.Slot,
					OutReport.Diagnostics,
					FString::Printf(TEXT("%s.slot"), *InNodePath));
				ForgePropertyCodec::PruneDefaults(SlotClass, InNode.Slot);
			}
		}

		// UMG 用控件名做匹配键，所以重名在这里是**硬错误**（通用层只给警告，见 ForgeDoc 那段注释）。
		TSet<FString> SeenNames;
		for (const FForgeNode& ChildConfig : InNode.Children)
		{
			bool bAlreadyPresent = false;
			SeenNames.Add(ChildConfig.Name, &bAlreadyPresent);
			if (bAlreadyPresent)
			{
				OutReport.AddError(
					FString::Printf(TEXT("%s.children"), *InNodePath),
					FString::Printf(
						TEXT("同一父节点下出现重名控件 `%s`；name 是导入时的匹配键，必须唯一"),
						*ChildConfig.Name));
			}
		}

		for (FForgeNode& Child : InNode.Children)
		{
			ValidateSubtree(Child, WidgetClass, JoinNodePath(InNodePath, Child.Name), OutReport);
		}
	}

	UWidget* CreateChildWidget(
		UPanelWidget* InParent,
		UWidgetTree* InTree,
		const FForgeNode& InNode,
		FForgeReport& OutReport,
		const FString& InNodePath)
	{
		FString ClassError;
		UClass* WidgetClass = UmgWidgetCodec::ResolveWidgetClass(InNode.ClassName, ClassError);
		if (WidgetClass == nullptr)
		{
			OutReport.AddError(JoinNodePath(InNodePath, TEXT("class")), ClassError);
			return nullptr;
		}

		UWidget* Widget = InTree->ConstructWidget<UWidget>(WidgetClass, FName(*InNode.Name));
		if (Widget == nullptr)
		{
			OutReport.AddError(InNodePath, FString::Printf(TEXT("创建控件 `%s` 失败"), *InNode.Name));
			return nullptr;
		}

		InParent->AddChild(Widget);
		NotifyVariableAdded(InTree, Widget);

		UmgWidgetCodec::ImportWidgetInto(Widget, InNode, OutReport.Diagnostics, InNodePath);

		if (UPanelWidget* ChildPanel = Cast<UPanelWidget>(Widget))
		{
			for (const FForgeNode& GrandChild : InNode.Children)
			{
				CreateChildWidget(ChildPanel, InTree, GrandChild, OutReport, JoinNodePath(InNodePath, GrandChild.Name));
			}
		}

		return Widget;
	}

	bool ReorderChildren(UPanelWidget* InPanel, const FForgeNode& InTarget)
	{
		bool bChanged = false;
		int32 DesiredIndex = 0;

		for (const FForgeNode& ChildNode : InTarget.Children)
		{
			int32 CurrentIndex = INDEX_NONE;
			const int32 ChildCount = InPanel->GetChildrenCount();
			for (int32 Index = 0; Index < ChildCount; ++Index)
			{
				UWidget* Child = InPanel->GetChildAt(Index);
				if (Child != nullptr && Child->GetName() == ChildNode.Name)
				{
					CurrentIndex = Index;
					break;
				}
			}

			if (CurrentIndex != INDEX_NONE && CurrentIndex != DesiredIndex)
			{
				InPanel->ShiftChild(DesiredIndex, InPanel->GetChildAt(CurrentIndex));
				bChanged = true;
			}

			++DesiredIndex;
		}

		return bChanged;
	}

	bool SyncWidget(
		UWidget* InWidget,
		const FForgeNode& InTarget,
		UWidgetTree* InTree,
		FForgeReport& OutReport,
		const FString& InNodePath)
	{
		if (InWidget == nullptr)
		{
			return false;
		}

		UmgWidgetCodec::ImportWidgetInto(InWidget, InTarget, OutReport.Diagnostics, InNodePath);

		if (UPanelWidget* Panel = Cast<UPanelWidget>(InWidget))
		{
			return SyncChildren(Panel, InTree, InTarget, OutReport, InNodePath);
		}

		if (InTarget.Children.Num() > 0)
		{
			OutReport.AddError(
				JoinNodePath(InNodePath, TEXT("children")),
				FString::Printf(
					TEXT("控件 `%s`（%s）不是面板，不能有子节点"),
					*InTarget.Name,
					*InTarget.ClassName));
		}

		return false;
	}

	bool SyncChildren(
		UPanelWidget* InPanel,
		UWidgetTree* InTree,
		const FForgeNode& InTarget,
		FForgeReport& OutReport,
		const FString& InNodePath)
	{
		bool bChanged = false;

		TMap<FString, UWidget*> CurrentByName;
		const int32 ChildCount = InPanel->GetChildrenCount();
		CurrentByName.Reserve(ChildCount);
		for (int32 Index = 0; Index < ChildCount; ++Index)
		{
			if (UWidget* Child = InPanel->GetChildAt(Index))
			{
				CurrentByName.Add(Child->GetName(), Child);
			}
		}

		TSet<FString> TargetNames;
		TargetNames.Reserve(InTarget.Children.Num());

		for (const FForgeNode& ChildNode : InTarget.Children)
		{
			TargetNames.Add(ChildNode.Name);
			const FString ChildPath = JoinNodePath(InNodePath, ChildNode.Name);

			UWidget** Existing = CurrentByName.Find(ChildNode.Name);
			if (Existing == nullptr || *Existing == nullptr)
			{
				bChanged |= CreateChildWidget(InPanel, InTree, ChildNode, OutReport, ChildPath) != nullptr;
				continue;
			}

			UWidget* ExistingWidget = *Existing;

			FString ClassError;
			UClass* DesiredClass = UmgWidgetCodec::ResolveWidgetClass(ChildNode.ClassName, ClassError);
			if (DesiredClass == nullptr)
			{
				OutReport.AddError(JoinNodePath(ChildPath, TEXT("class")), ClassError);
				continue;
			}

			// 类型变了没法就地改：删掉重建。子控件会随旧控件一起消失——这是换类型的必然代价，
			// 计划里也如实标成「先删后建」，不假装能保留。
			if (!ExistingWidget->IsA(DesiredClass))
			{
				// 先摘干净：只从面板里 RemoveChild 不够，对象与名字还在，同名构造会 fatal。
				DetachWidgetSubtree(InTree, ExistingWidget);
				bChanged |= CreateChildWidget(InPanel, InTree, ChildNode, OutReport, ChildPath) != nullptr;
				continue;
			}

			bChanged |= SyncWidget(ExistingWidget, ChildNode, InTree, OutReport, ChildPath);
		}

		// 配置里没有的控件：删。
		// 这一步是「配置是唯一真源」的执行点——想在编辑器里保留手工加的控件，
		// 就得先把它导出回配置，而不是指望导入时被放过。
		for (int32 Index = InPanel->GetChildrenCount() - 1; Index >= 0; --Index)
		{
			UWidget* Child = InPanel->GetChildAt(Index);
			if (Child != nullptr && !TargetNames.Contains(Child->GetName()))
			{
				NotifyVariableRemoved(InTree, Child);
				InPanel->RemoveChildAt(Index);
				bChanged = true;
			}
		}

		bChanged |= ReorderChildren(InPanel, InTarget);

		return bChanged;
	}
}

bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport)
{
	OutReport.AssetPath = InAssetPath;

	UWidgetBlueprint* Blueprint = FindBlueprint(InAssetPath);
	if (Blueprint == nullptr)
	{
		OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("找不到 UMG 资产：%s"), *InAssetPath));
		return false;
	}

	if (Blueprint->WidgetTree == nullptr || Blueprint->WidgetTree->RootWidget == nullptr)
	{
		OutReport.AddError(TEXT("root"), FString::Printf(TEXT("资产 %s 没有根控件，无法导出"), *InAssetPath));
		return false;
	}

	OutDocument.Kind = Kind;
	OutDocument.SchemaVersion = ForgeDoc::CurrentSchemaVersion;
	OutDocument.Asset = InAssetPath;
	OutDocument.Parent = (Blueprint->ParentClass != nullptr) ? Blueprint->ParentClass->GetPathName() : FString();

	UmgWidgetCodec::ExportWidget(
		Blueprint->WidgetTree->RootWidget,
		OutDocument.Root,
		OutReport.Diagnostics,
		TEXT("root"));

	// 动画走顶层字段（文档的 Extra），不写进控件树——它不是树的层级节点。
	UmgAnimationCodec::ExportToDocument(*Blueprint, OutDocument, OutReport.Diagnostics);

	OutReport.bSucceeded = true;
	return true;
}

bool ReadAssetTree(const FString& InAssetPath, FForgeNode& OutRoot, FForgeReport& OutReport)
{
	OutReport.AssetPath = InAssetPath;

	UWidgetBlueprint* Blueprint = FindBlueprint(InAssetPath);
	if (Blueprint == nullptr || Blueprint->WidgetTree == nullptr || Blueprint->WidgetTree->RootWidget == nullptr)
	{
		OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("资产 %s 不存在或没有根控件"), *InAssetPath));
		return false;
	}

	UmgWidgetCodec::ExportWidget(
		Blueprint->WidgetTree->RootWidget,
		OutRoot,
		OutReport.Diagnostics,
		TEXT("root"));

	OutReport.bSucceeded = true;
	return true;
}

bool ImportDocument(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport)
{
	OutReport.bDryRun = bDryRun;
	OutReport.SourcePath = InDocument.SourcePath;
	OutReport.AssetPath = InDocument.Asset;

	UWidgetBlueprint* Blueprint = FindBlueprint(InDocument.Asset);

	// 父类在计划阶段就解析：解析不了（例如 UnrealSharp 的 C# 类还没加载）属于"配置与当前
	// 环境不匹配"——必须以一条报告返回，而不是等创建/编译时才失败（那时资产可能已被改动）。
	FString ParentClassError;
	UClass* ParentClass = ResolveParentClass(InDocument.Parent, ParentClassError);
	if (ParentClass == nullptr)
	{
		OutReport.AddError(TEXT("parent"), ParentClassError);
		return false;
	}

	// 先算计划：真跑与 dry-run 走同一条判定路径，两者的结论必然一致。
	const bool bHasCurrentTree = Blueprint != nullptr
		&& Blueprint->WidgetTree != nullptr
		&& Blueprint->WidgetTree->RootWidget != nullptr;

	if (bHasCurrentTree)
	{
		FForgeNode CurrentRoot;
		UmgWidgetCodec::ExportWidget(
			Blueprint->WidgetTree->RootWidget,
			CurrentRoot,
			OutReport.Diagnostics,
			TEXT("root"));
		ForgeDiff::ComputeRootPlan(InDocument.Root, CurrentRoot, OutReport.Actions);
	}
	else
	{
		MarkSubtreeCreate(InDocument.Root, TEXT("root"), OutReport.Actions);
	}

	// 动画：解析配置（含对着配置树做的字段与路径校验），再与资产现状一比得出计划。
	// 与控件树走同一条「先算计划」的路径，dry-run 与真跑的结论因此一致。
	TArray<UmgAnimationCodec::FUmgAnimation> TargetAnimations;
	TArray<UmgAnimationCodec::FUmgAnimation> CurrentAnimations;
	if (!UmgAnimationCodec::ParseAnimations(InDocument, TargetAnimations, OutReport))
	{
		return false;
	}
	if (Blueprint != nullptr)
	{
		UmgAnimationCodec::ReadAnimations(*Blueprint, CurrentAnimations, OutReport.Diagnostics);
	}
	UmgAnimationCodec::ComputePlan(TargetAnimations, CurrentAnimations, OutReport.Actions);

	OutReport.CountActions();

	// 属性校验同样在计划阶段做完：带着错误去改资产会留下改到一半的状态，
	// 而「先建包、再失败」更糟——包已经建好却没能保存。
	ValidateSubtree(InDocument.Root, /*InParentWidgetClass=*/nullptr, TEXT("root"), OutReport);

	if (bDryRun)
	{
		OutReport.bSucceeded = !OutReport.HasErrors();
		return OutReport.bSucceeded;
	}

	// 计划阶段就出错（属性名不存在、控件类解析不了……）时不要带着错误去改资产：
	// 改到一半的资产比干脆失败更难收拾。
	if (OutReport.HasErrors())
	{
		return false;
	}

	if (Blueprint == nullptr)
	{
		FString CreateError;
		Blueprint = CreateBlueprint(InDocument, ParentClass, CreateError);
		if (Blueprint == nullptr)
		{
			OutReport.AddError(TEXT("asset"), CreateError);
			return false;
		}
	}
	else if (Blueprint->ParentClass != ParentClass)
	{
		// 父类不匹配就停：UMG 编译器对"换父类"的资产很敏感（骨架类/变量表都会重建），
		// 与其半路改坏，不如报一条报告让作者人工处理。
		OutReport.AddError(
			TEXT("parent"),
			FString::Printf(
				TEXT("现有蓝图的父类是 `%s`，与配置里的 `%s` 不一致；换父类请人工处理"),
				Blueprint->ParentClass != nullptr ? *Blueprint->ParentClass->GetPathName() : TEXT("None"),
				*InDocument.Parent));
		return false;
	}

	UWidgetTree* Tree = Blueprint->WidgetTree;
	if (Tree == nullptr)
	{
		OutReport.AddError(TEXT("asset"), TEXT("UWidgetBlueprint 没有 WidgetTree"));
		return false;
	}

	FString RootClassError;
	UClass* RootClass = UmgWidgetCodec::ResolveWidgetClass(InDocument.Root.ClassName, RootClassError);
	if (RootClass == nullptr)
	{
		OutReport.AddError(TEXT("root.class"), RootClassError);
		return false;
	}

	UWidget* Root = Tree->RootWidget;
	if (Root == nullptr || !Root->IsA(RootClass) || Root->GetName() != InDocument.Root.Name)
	{
		// 根控件换了名字或类型：整棵重建。根是这棵树的身份，它变了就没有「按名保留」可言。
		// 旧根（连同子树）必须先摘干净并改名挪走，否则同名构造同样会 fatal。
		if (Root != nullptr)
		{
			DetachWidgetSubtree(Tree, Root);
			Tree->RootWidget = nullptr;
		}
		Root = Tree->ConstructWidget<UWidget>(RootClass, FName(*InDocument.Root.Name));
		Tree->RootWidget = Root;

		// 根也要登记进「变量名 → GUID」表（子控件由 CreateChildWidget 登记）。
		// 漏了它，首次编译会报 ensure：`Widget [X] was added but did not get a GUID`
		// ——编译并不会失败，但每次新建的 WBP 都要先吃一条 Error 与一份转储。
		NotifyVariableAdded(Tree, Root);
	}

	if (Root == nullptr)
	{
		OutReport.AddError(TEXT("root"), TEXT("创建根控件失败"));
		return false;
	}

	SyncWidget(Root, InDocument.Root, Tree, OutReport, TEXT("root"));

	if (OutReport.HasErrors())
	{
		return false;
	}

	// 动画：按名增量——命中整体重建该动画的 MovieScene 与绑定、缺的建、多的删。
	if (!UmgAnimationCodec::Sync(*Blueprint, TargetAnimations, OutReport.Diagnostics))
	{
		return false;
	}

	// 没有动作就不要写盘：无意义的保存会改时间戳、触发下游重导，也让「改动」这件事失真。
	if (OutReport.Actions.Num() == 0)
	{
		OutReport.bSucceeded = true;
		return true;
	}

	// 编译前把「变量名 → GUID」表与控件树对齐：UMG 编译器对这张表的两条 ensure 在本项目会
	// 升级成崩溃（编辑器直接退出），这里统一收口——导入失败最多是一条报告，不会带走编辑器。
	AlignVariableGuidMap(*Blueprint);

	// 编译会重建生成类与「变量名 → GUID」映射。不编译就保存出去的是半成品蓝图，
	// 而 UMG 的变量 GUID 只在编译时补齐；漏了它，下次加载会重新生成，动画引用随之断掉。
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	UPackage* Package = Blueprint->GetOutermost();
	Package->MarkPackageDirty();

	// 必须自己算出磁盘文件名：新建的包在第一次保存前 FileName 是空的，
	// 直接把包名（`/Game/...`）当文件名传进去会「成功」但什么都不写。
	const FString PackageFileName = FPaths::ConvertRelativePathToFull(
		FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()));

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, Blueprint, *PackageFileName, SaveArgs))
	{
		OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("保存资产失败：%s"), *PackageFileName));
		return false;
	}

	OutReport.bSucceeded = true;
	return true;
}

} // namespace UmgForge
