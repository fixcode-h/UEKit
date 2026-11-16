// Copyright Epic Games, Inc. All Rights Reserved.

#include "Kinds/Material/MaterialForge.h"
#include "AssetForgeCompat.h"

#include "Kinds/Material/MaterialGraphCodec.h"
#include "Core/ForgeDiff.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgePropertyCodec.h"

#include "AssetToolsModule.h"
#include "Factories/MaterialFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditorUtilities.h"
#include "MaterialEditingLibrary.h"
#include "MaterialGraph/MaterialGraph.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "MaterialShared.h"
#include "RHIDefinitions.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "SceneTypes.h"
#include "UObject/SavePackage.h"

namespace MaterialForge
{

namespace
{
	// 处理器是这套类型的主要消费者，名字在这里短一截更好读。
	using MaterialGraphCodec::FMaterialGraph;
	using MaterialGraphCodec::FMaterialLink;
	using MaterialGraphCodec::FMaterialNodeSpec;
	/** 没连任何东西的表达式（孤儿节点）的摆放：四行一列，列往左。 */
	constexpr int32 OrphanRowsPerColumn = 4;
	constexpr int32 OrphanRowSpacing = 200;
	constexpr int32 OrphanColumnSpacing = 320;

	/** 两个属性集合是否等价——「没写」与「空的」视为同一件事，理由见 ForgeDiff 里的同款判定。 */
	bool ArePropsEquivalent(const TSharedPtr<FJsonObject>& InLeft, const TSharedPtr<FJsonObject>& InRight)
	{
		const bool bLeftEmpty = !InLeft.IsValid() || InLeft->Values.Num() == 0;
		const bool bRightEmpty = !InRight.IsValid() || InRight->Values.Num() == 0;
		if (bLeftEmpty && bRightEmpty)
		{
			return true;
		}

		return ForgeJsonText::ToCanonicalString(InLeft) == ForgeJsonText::ToCanonicalString(InRight);
	}

	/** 资产路径 → 已加载的材质；不存在返回 nullptr。 */
	UMaterial* FindMaterial(const FString& InAssetPath)
	{
		if (InAssetPath.IsEmpty())
		{
			return nullptr;
		}

		return Cast<UMaterial>(StaticLoadObject(UMaterial::StaticClass(), nullptr, *InAssetPath));
	}

	/** `root.class` → 材质的类：`Material` 是短名，其余按完整路径解析（UMaterial 的 C++ 子类）。 */
	UClass* ResolveMaterialClass(const FString& InClassName, FString& OutError)
	{
		if (InClassName.IsEmpty()
			|| InClassName.Equals(MaterialClass, ESearchCase::IgnoreCase)
			|| InClassName.Equals(TEXT("UMaterial"), ESearchCase::IgnoreCase))
		{
			return UMaterial::StaticClass();
		}

		if (UClass* Loaded = LoadClass<UMaterial>(nullptr, *InClassName))
		{
			return Loaded;
		}

		OutError = FString::Printf(
			TEXT("`%s` 不是材质类；这个 kind 只认 UMaterial（材质实例是另一种资产，不进这里）"),
			*InClassName);
		return nullptr;
	}

	/** 一条连线的文本形式：`Tex` 与 `{ from: Tex, output: R }` 要能判出不同。 */
	FString MakeLinkKey(const FMaterialLink& InLink)
	{
		return InLink.OutputName.IsEmpty()
			? InLink.NodeName
			: FString::Printf(TEXT("%s.%s"), *InLink.NodeName, *InLink.OutputName);
	}

	/** 两组连线之间的差异，形如 `inputs.A: Tex → Tex2`。没有差异返回空串。 */
	FString DescribeLinkDiff(
		const TArray<TPair<FString, FMaterialLink>>& InTarget,
		const TArray<TPair<FString, FMaterialLink>>& InCurrent,
		const FString& InPrefix)
	{
		TMap<FString, FString> TargetKeys;
		TargetKeys.Reserve(InTarget.Num());
		for (const TPair<FString, FMaterialLink>& Pair : InTarget)
		{
			TargetKeys.Add(Pair.Key, MakeLinkKey(Pair.Value));
		}

		TMap<FString, FString> CurrentKeys;
		CurrentKeys.Reserve(InCurrent.Num());
		for (const TPair<FString, FMaterialLink>& Pair : InCurrent)
		{
			CurrentKeys.Add(Pair.Key, MakeLinkKey(Pair.Value));
		}

		TArray<FString> Parts;
		for (const TPair<FString, FString>& Pair : TargetKeys)
		{
			const FString* Existing = CurrentKeys.Find(Pair.Key);
			if (Existing == nullptr)
			{
				Parts.Add(FString::Printf(TEXT("%s.%s（仅配置：%s）"), *InPrefix, *Pair.Key, *Pair.Value));
			}
			else if (*Existing != Pair.Value)
			{
				Parts.Add(FString::Printf(TEXT("%s.%s（配置 %s → 资产 %s）"), *InPrefix, *Pair.Key, *Pair.Value, **Existing));
			}
		}

		for (const TPair<FString, FString>& Pair : CurrentKeys)
		{
			if (!TargetKeys.Contains(Pair.Key))
			{
				Parts.Add(FString::Printf(TEXT("%s.%s（仅资产：%s）"), *InPrefix, *Pair.Key, *Pair.Value));
			}
		}

		Parts.Sort();
		return FString::Join(Parts, TEXT(", "));
	}

	/**
	 * 计划：目标图与当前图逐节点比。
	 *
	 * 两边都必须是**结构路径命名**的图（见 MaterialGraphCodec 的文件头），否则手写配置里的
	 * 短名会把每个节点都报成「先删后建」。bOutGraphChanged 说的是「要不要重建图」——
	 * 只有材质自身属性变了的话，图一个字都不用动。
	 */
	void ComputeGraphPlan(
		const FMaterialGraph& InTarget,
		const FMaterialGraph& InCurrent,
		TArray<FForgePlanEntry>& OutPlan,
		bool& bOutGraphChanged)
	{
		for (const FMaterialNodeSpec& TargetNode : InTarget.Nodes)
		{
			const FString NodePath = FString::Printf(TEXT("root/%s"), *TargetNode.Name);
			const FMaterialNodeSpec* CurrentNode = InCurrent.FindNode(TargetNode.Name);

			if (CurrentNode == nullptr)
			{
				OutPlan.Add({ EForgeAction::Create, NodePath, TargetNode.Name, FString::Printf(TEXT("新建 %s"), *TargetNode.ClassName) });
				bOutGraphChanged = true;
				continue;
			}

			if (CurrentNode->ClassName != TargetNode.ClassName)
			{
				OutPlan.Add({
					EForgeAction::Update,
					NodePath,
					TargetNode.Name,
					FString::Printf(TEXT("class: %s → %s（换类只能重建这个节点）"), *CurrentNode->ClassName, *TargetNode.ClassName) });
				bOutGraphChanged = true;
				continue;
			}

			TArray<FString> Details;
			if (!ArePropsEquivalent(TargetNode.Props, CurrentNode->Props))
			{
				Details.Add(ForgeDiff::DescribeObjectDiff(TargetNode.Props, CurrentNode->Props, TEXT("props")));
			}

			const FString LinkDetail = DescribeLinkDiff(TargetNode.Inputs, CurrentNode->Inputs, TEXT("inputs"));
			if (!LinkDetail.IsEmpty())
			{
				Details.Add(LinkDetail);
			}

			if (Details.Num() > 0)
			{
				OutPlan.Add({ EForgeAction::Update, NodePath, TargetNode.Name, FString::Join(Details, TEXT("; ")) });
				bOutGraphChanged = true;
			}
		}

		for (const FMaterialNodeSpec& CurrentNode : InCurrent.Nodes)
		{
			if (InTarget.FindNode(CurrentNode.Name) == nullptr)
			{
				OutPlan.Add({ EForgeAction::Delete, FString::Printf(TEXT("root/%s"), *CurrentNode.Name), CurrentNode.Name, TEXT("配置中已移除") });
				bOutGraphChanged = true;
			}
		}

		// 材质属性输入：它属于材质自己，接线变了也是一条「更新」。
		const FString PropertyDetail = DescribeLinkDiff(InTarget.PropertyInputs, InCurrent.PropertyInputs, TEXT("inputs"));
		if (!PropertyDetail.IsEmpty())
		{
			OutPlan.Add({ EForgeAction::Update, TEXT("root"), TEXT("材质属性输入"), PropertyDetail });
			bOutGraphChanged = true;
		}
	}

	/** 清空表达式集合。删除时引擎会顺手断开指向它们的连线（含材质属性输入）。 */
	void ClearExpressions(UMaterial* InMaterial)
	{
		// 先复制一份再删：集合在删除过程中会变短，直接迭代它的视图会读到已经移位的槽位。
		TArray<TObjectPtr<UMaterialExpression>> Existing;
		Existing.Append(InMaterial->GetExpressions());

		for (const TObjectPtr<UMaterialExpression>& Expression : Existing)
		{
			if (Expression != nullptr)
			{
				UMaterialEditingLibrary::DeleteMaterialExpression(InMaterial, Expression);
			}
		}
	}

	/** 材质自身的属性：先重置回 CDO（配置没写的就不该残留），再写配置里的。 */
	void ApplyMaterialProperties(UMaterial* InMaterial, const TSharedPtr<FJsonObject>& InProps, FForgeReport& OutReport)
	{
		ForgePropertyCodec::ResetToDefaults(InMaterial);
		ForgePropertyCodec::ImportProperties(InMaterial, InProps, OutReport.Diagnostics, TEXT("root.props"));

		// `ShadingModel`（单数）是细节面板里那个下拉，但编译只看私有的 `ShadingModels` 位域：
		// 写属性不会同步它，只有引擎自己的 SetShadingModel 会。少了这一步，配置里改了着色模型
		// 会「导入成功但没生效」——最坏的那种失败。
		const FProperty* ShadingModelProperty = FindFProperty<FProperty>(UMaterial::StaticClass(), TEXT("ShadingModel"));
		if (const FNumericProperty* Numeric = CastField<FNumericProperty>(ShadingModelProperty))
		{
			const int64 Raw = static_cast<int64>(Numeric->GetUnsignedIntPropertyValue_InContainer(InMaterial));
			if (Raw >= 0 && Raw < MSM_NUM)
			{
				InMaterial->SetShadingModel(static_cast<EMaterialShadingModel>(Raw));
			}
		}
	}

	/** 从材质属性输入出发能走到的表达式——排过版的那批；剩下的就是孤儿。 */
	void CollectReachable(UMaterial* InMaterial, TSet<const UMaterialExpression*>& OutReachable)
	{
		TArray<const UMaterialExpression*> Stack;
		for (int32 PropertyIndex = 0; PropertyIndex < MP_MAX; ++PropertyIndex)
		{
			if (const FExpressionInput* Input = InMaterial->GetExpressionInputForProperty(static_cast<EMaterialProperty>(PropertyIndex)))
			{
				if (Input->Expression != nullptr)
				{
					Stack.Push(Input->Expression);
				}
			}
		}

		while (Stack.Num() > 0)
		{
			const UMaterialExpression* Expression = Stack.Pop();

			bool bAlreadyVisited = false;
			OutReachable.Add(Expression, &bAlreadyVisited);
			if (bAlreadyVisited)
			{
				continue;
			}

			for (int32 InputIndex = 0; InputIndex < ForgeCountInputs(Expression); ++InputIndex)
			{
				if (const FExpressionInput* Input = ForgeGetInput(Expression, InputIndex))
				{
					if (Input->Expression != nullptr)
					{
						Stack.Push(Input->Expression);
					}
				}
			}
		}
	}

	/**
	 * 排版。
	 *
	 * 连上材质属性的那批交给**引擎自己的「重整节点」**（按到输出的深度分列、同列里按高度往下排）；
	 * 孤儿节点它不管（`LayoutMaterialExpressions` 只从材质属性输入出发），不另摆就会全堆在 (0,0)。
	 */
	void LayoutExpressions(UMaterial* InMaterial)
	{
		UMaterialEditingLibrary::LayoutMaterialExpressions(InMaterial);

		TSet<const UMaterialExpression*> Reachable;
		CollectReachable(InMaterial, Reachable);

		TArray<UMaterialExpression*> Orphans;
		int32 LeftmostX = 0;
		for (const TObjectPtr<UMaterialExpression>& Expression : InMaterial->GetExpressions())
		{
			if (Expression == nullptr)
			{
				continue;
			}

			if (Reachable.Contains(Expression))
			{
				LeftmostX = FMath::Min(LeftmostX, Expression->MaterialExpressionEditorX);
			}
			else
			{
				Orphans.Add(Expression);
			}
		}

		int32 Column = 1;
		int32 Row = 0;
		for (UMaterialExpression* Orphan : Orphans)
		{
			Orphan->MaterialExpressionEditorX = LeftmostX - OrphanColumnSpacing * Column;
			Orphan->MaterialExpressionEditorY = Row * OrphanRowSpacing;

			if (++Row >= OrphanRowsPerColumn)
			{
				Row = 0;
				++Column;
			}
		}
	}

	/**
	 * 材质编辑器开着的话，它手边那张图是表达式集合的一份快照——清空重建之后必须让它重建一次，
	 * 否则屏幕上还是旧图，而且随后的交互会把旧状态写回表达式。
	 */
	void RefreshOpenEditor(UMaterial* InMaterial)
	{
		if (InMaterial->MaterialGraph == nullptr)
		{
			return;
		}

		InMaterial->MaterialGraph->RebuildGraph();
		FMaterialEditorUtilities::UpdateMaterialAfterGraphChange(InMaterial->MaterialGraph);
	}

	/** 整体重建：清空 → 按配置建 → 写属性 → 连线 → 排版。 */
	bool RebuildGraph(UMaterial* InMaterial, const FMaterialGraph& InGraph, FForgeReport& OutReport)
	{
		ClearExpressions(InMaterial);

		// 建节点走引擎自己的创建入口：函数调用的参数、动态参数、组件掩码这些「创建时要点一把」
		// 的东西它都安顿好了（位置给 0，最后统一排版）。
		TMap<FString, UMaterialExpression*> ByName;
		ByName.Reserve(InGraph.Nodes.Num());

		for (const FMaterialNodeSpec& Node : InGraph.Nodes)
		{
			FString ClassError;
			UClass* ExpressionClass = MaterialGraphCodec::ResolveExpressionClass(Node.ClassName, ClassError);
			if (ExpressionClass == nullptr)
			{
				OutReport.AddError(FString::Printf(TEXT("root/%s.class"), *Node.Name), ClassError);
				continue;
			}

			UMaterialExpression* Expression = UMaterialEditingLibrary::CreateMaterialExpression(
				InMaterial, ExpressionClass, /*NodePosX=*/0, /*NodePosY=*/0);

			if (Expression == nullptr)
			{
				OutReport.AddError(FString::Printf(TEXT("root/%s"), *Node.Name), FString::Printf(TEXT("创建 %s 失败"), *Node.ClassName));
				continue;
			}

			ByName.Add(Node.Name, Expression);
		}

		// 属性：新建的节点本来就是默认值，不用先重置，直接写。
		for (const FMaterialNodeSpec& Node : InGraph.Nodes)
		{
			if (UMaterialExpression* const* Expression = ByName.Find(Node.Name))
			{
				ForgePropertyCodec::ImportProperties(
					*Expression, Node.Props, OutReport.Diagnostics, FString::Printf(TEXT("root/%s.props"), *Node.Name));
			}
		}

		// 连线：节点都建完之后再连，源节点一定已经在了。
		for (const FMaterialNodeSpec& Node : InGraph.Nodes)
		{
			UMaterialExpression* const* Target = ByName.Find(Node.Name);
			if (Target == nullptr)
			{
				continue;
			}

			for (const TPair<FString, FMaterialLink>& Pair : Node.Inputs)
			{
				const FString FieldPath = FString::Printf(TEXT("root/%s.inputs.%s"), *Node.Name, *Pair.Key);

				UMaterialExpression* const* Source = ByName.Find(Pair.Value.NodeName);
				if (Source == nullptr)
				{
					OutReport.AddError(FieldPath, FString::Printf(TEXT("源节点 `%s` 没建出来"), *Pair.Value.NodeName));
					continue;
				}

				const FString SourceText = MakeLinkKey(Pair.Value);
				const int32 OutputIndex = MaterialGraphCodec::ResolveOutputIndex(*Source, Pair.Value.OutputName);
				const int32 InputIndex = MaterialGraphCodec::ResolveInputIndex(*Target, Pair.Key);
				FExpressionInput* Input = (InputIndex != INDEX_NONE) ? (*Target)->GetInput(InputIndex) : nullptr;

				if (OutputIndex == INDEX_NONE || Input == nullptr)
				{
					OutReport.AddError(
						FieldPath,
						FString::Printf(TEXT("连不上：%s → %s.%s"), *SourceText, *Node.Name, *Pair.Key));
					continue;
				}

				// 与 UMaterialEditingLibrary::ConnectMaterialExpressions 是同一步：引脚序号 + 源表达式。
				// 自己写这一步，是因为 `#序号` 那种没名字的引脚按名字找不到。
				Input->Connect(OutputIndex, *Source);
			}
		}

		// 材质属性输入。
		for (const TPair<FString, FMaterialLink>& Pair : InGraph.PropertyInputs)
		{
			const FString FieldPath = FString::Printf(TEXT("root.inputs.%s"), *Pair.Key);
			const int32 Property = MaterialGraphCodec::ResolveMaterialProperty(Pair.Key);
			UMaterialExpression* const* Source = ByName.Find(Pair.Value.NodeName);

			if (Property == INDEX_NONE || Source == nullptr)
			{
				OutReport.AddError(FieldPath, FString::Printf(TEXT("材质属性连不上：节点 `%s` 没建出来"), *Pair.Value.NodeName));
				continue;
			}

			if (!UMaterialEditingLibrary::ConnectMaterialProperty(*Source, Pair.Value.OutputName, static_cast<EMaterialProperty>(Property)))
			{
				OutReport.AddError(
					FieldPath,
					FString::Printf(TEXT("材质属性连不上：%s 的输出拖不到 %s"), *MakeLinkKey(Pair.Value), *Pair.Key));
			}
		}

		LayoutExpressions(InMaterial);
		RefreshOpenEditor(InMaterial);
		return !OutReport.HasErrors();
	}

	UMaterial* CreateMaterial(const FString& InAssetPath, UClass* InMaterialClass, FString& OutError)
	{
		FString PackagePath;
		FString AssetName;
		InAssetPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (AssetName.IsEmpty() || PackagePath.IsEmpty())
		{
			OutError = FString::Printf(TEXT("资产路径 `%s` 不合法，应形如 /Game/Materials/M_Xxx"), *InAssetPath);
			return nullptr;
		}

		// 走 IAssetTools + 引擎自己的工厂：包创建、AssetCreated 广播、命名冲突都由它兜住，
		// 与项目里 Tools/import-qtank.py 的 assets.create_asset(..., MaterialFactoryNew()) 同一条路。
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
		UObject* Created = AssetTools.CreateAsset(AssetName, PackagePath, InMaterialClass, Factory);

		UMaterial* Material = Cast<UMaterial>(Created);
		if (Material == nullptr)
		{
			OutError = FString::Printf(TEXT("创建材质失败：%s"), *InAssetPath);
		}

		return Material;
	}
}

bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport)
{
	OutReport.AssetPath = InAssetPath;

	UMaterial* Material = FindMaterial(InAssetPath);
	if (Material == nullptr)
	{
		OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("找不到材质资产：%s"), *InAssetPath));
		return false;
	}

	OutDocument.Kind = Kind;
	OutDocument.SchemaVersion = ForgeDoc::CurrentSchemaVersion;
	OutDocument.Asset = InAssetPath;
	OutDocument.Root.ClassName = MaterialClass;
	OutDocument.Root.Name = Material->GetName();

	ForgePropertyCodec::ExportDeltaProperties(Material, OutDocument.Root.Props, OutReport.Diagnostics, TEXT("root.props"));

	FMaterialGraph Graph;
	if (!MaterialGraphCodec::ReadGraph(Material, Graph, OutReport.Diagnostics))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Extra = MakeShared<FJsonObject>();
	MaterialGraphCodec::WriteGraph(Graph, Extra);
	OutDocument.Root.Extra = Extra;

	OutReport.bSucceeded = true;
	return true;
}

bool ImportDocument(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport)
{
	OutReport.bDryRun = bDryRun;
	OutReport.SourcePath = InDocument.SourcePath;
	OutReport.AssetPath = InDocument.Asset;

	FString ClassError;
	UClass* MaterialAssetClass = ResolveMaterialClass(InDocument.Root.ClassName, ClassError);
	if (MaterialAssetClass == nullptr)
	{
		OutReport.AddError(TEXT("root.class"), ClassError);
		return false;
	}

	// 计划阶段做完所有校验：带着错误去改资产会留下改到一半的状态。
	ForgePropertyCodec::ValidateProperties(MaterialAssetClass, InDocument.Root.Props, OutReport.Diagnostics, TEXT("root.props"));
	ForgePropertyCodec::PruneDefaults(MaterialAssetClass, InDocument.Root.Props);

	FMaterialGraph TargetGraph;
	if (!MaterialGraphCodec::ParseGraph(InDocument.Root.Extra, TargetGraph, OutReport.Diagnostics, TEXT("root")))
	{
		return false;
	}

	MaterialGraphCodec::ValidateGraph(TargetGraph, OutReport.Diagnostics, TEXT("root"));

	UMaterial* Material = FindMaterial(InDocument.Asset);
	bool bGraphChanged = Material == nullptr;

	if (Material != nullptr)
	{
		FMaterialGraph CurrentGraph;
		if (!MaterialGraphCodec::ReadGraph(Material, CurrentGraph, OutReport.Diagnostics))
		{
			return false;
		}

		TSharedPtr<FJsonObject> CurrentProps;
		ForgePropertyCodec::ExportDeltaProperties(Material, CurrentProps, OutReport.Diagnostics, TEXT("root.props"));

		// 判等走结构路径：配置里的短名与导出形态在这之后没有区别。
		MaterialGraphCodec::CanonicalizeNames(TargetGraph);
		ComputeGraphPlan(TargetGraph, CurrentGraph, OutReport.Actions, bGraphChanged);

		if (!ArePropsEquivalent(InDocument.Root.Props, CurrentProps))
		{
			OutReport.Actions.Add({
				EForgeAction::Update,
				TEXT("root"),
				InDocument.Root.Name,
				ForgeDiff::DescribeObjectDiff(InDocument.Root.Props, CurrentProps, TEXT("props")) });
		}
	}
	else
	{
		OutReport.Actions.Add({ EForgeAction::Create, TEXT("root"), InDocument.Root.Name, FString::Printf(TEXT("新建材质 %s"), *InDocument.Asset) });
		for (const FMaterialNodeSpec& Node : TargetGraph.Nodes)
		{
			OutReport.Actions.Add({ EForgeAction::Create, FString::Printf(TEXT("root/%s"), *Node.Name), Node.Name, FString::Printf(TEXT("新建 %s"), *Node.ClassName) });
		}
	}

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

	// 没有动作就不碰资产：无意义的重建会换掉表达式对象、改文件时间戳，
	// 「哪里变了」这件事也跟着失真。
	if (OutReport.Actions.Num() == 0)
	{
		OutReport.bSucceeded = true;
		return true;
	}

	if (Material == nullptr)
	{
		FString CreateError;
		Material = CreateMaterial(InDocument.Asset, MaterialAssetClass, CreateError);
		if (Material == nullptr)
		{
			OutReport.AddError(TEXT("asset"), CreateError);
			return false;
		}
	}

	ApplyMaterialProperties(Material, InDocument.Root.Props, OutReport);

	// 只改了材质自身属性的话，图一个字都不用动（重建会换掉所有表达式对象、连带着把
	// 手工摆过的节点位置也抹掉）。
	if (bGraphChanged)
	{
		if (!RebuildGraph(Material, TargetGraph, OutReport))
		{
			return false;
		}
	}

	// 编译：错误当失败，不写盘——把编译不过的材质存下来比留在内存里更糟。
#if ASSETFORGE_RECOMPILE_RETURNS_ERRORS
	const TArray<FString> CompileErrors = UMaterialEditingLibrary::RecompileMaterial(Material);
#else
	// 5.4 的 RecompileMaterial 不回错误：等编辑器用的那份材质资源编完，再从它上面取。
	UMaterialEditingLibrary::RecompileMaterial(Material);
	TArray<FString> CompileErrors;
	if (FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform))
	{
		Resource->FinishCompilation();
		CompileErrors = Resource->GetCompileErrors();
	}
#endif
	for (const FString& CompileError : CompileErrors)
	{
		OutReport.AddError(TEXT("root"), FString::Printf(TEXT("编译错误：%s"), *CompileError));
	}

	if (OutReport.HasErrors())
	{
		return false;
	}

	UPackage* Package = Material->GetOutermost();
	Package->MarkPackageDirty();

	// 新建的包在第一次保存前 FileName 是空的，磁盘文件名必须自己算（理由同 UMG 处理器）。
	const FString PackageFileName = FPaths::ConvertRelativePathToFull(
		FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()));

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, Material, *PackageFileName, SaveArgs))
	{
		OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("保存资产失败：%s"), *PackageFileName));
		return false;
	}

	OutReport.bSucceeded = true;
	return true;
}

} // namespace MaterialForge
