// Copyright Epic Games, Inc. All Rights Reserved.

#include "Kinds/Object/ObjectBlueprintForge.h"

#include "Core/ForgeClassResolver.h"
#include "Core/ForgeDiff.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgePropertyCodec.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace ObjectBlueprintForge
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

	/**
	 * 解析父类并做"非 Actor 蓝图"的三项校验——每一项都给出可操作的报错，
	 * 而不是含糊的"父类不合法"。
	 */
	UClass* ResolveParentClass(const FString& InParentPath, FString& OutError)
	{
		if (InParentPath.IsEmpty())
		{
			OutError = TEXT("缺少 parent：需要指明蓝图父类（CDO 属性写在它的默认值之上）");
			return nullptr;
		}

		// 通用解析（含 C# 类兜底）：这里要求的最低基类是 UObject，AActor 的拒绝放在下面单独判，
		// 好把"该用哪个 kind"讲清楚。
		UClass* ParentClass = FForgeClassResolver::Resolve(InParentPath, UObject::StaticClass(), OutError);
		if (ParentClass == nullptr)
		{
			return nullptr;
		}

		if (ParentClass->IsChildOf(AActor::StaticClass()))
		{
			OutError = FString::Printf(
				TEXT("`%s` 是 AActor 派生类——它属于 actor-blueprint（组件树那一侧）"),
				*InParentPath);
			return nullptr;
		}

		if (ParentClass->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = FString::Printf(TEXT("`%s` 是抽象类，建不出实例"), *InParentPath);
			return nullptr;
		}

		// "能不能当蓝图父类"的规则收在引擎工具函数里（可蓝图化、类型限制都在内）。
		if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
		{
			OutError = FString::Printf(TEXT("`%s` 不能作为蓝图父类（不可蓝图化或类型不允许）"), *InParentPath);
			return nullptr;
		}

		return ParentClass;
	}

	/** 承载默认值的对象：优先蓝图生成类的 CDO，生成类缺失时回退父类 CDO。 */
	UObject* ResolveClassDefaultObject(const UBlueprint* InBlueprint)
	{
		if (InBlueprint == nullptr)
		{
			return nullptr;
		}

		if (InBlueprint->GeneratedClass != nullptr)
		{
			return InBlueprint->GeneratedClass->GetDefaultObject();
		}

		if (InBlueprint->ParentClass != nullptr)
		{
			return InBlueprint->ParentClass->GetDefaultObject();
		}

		return nullptr;
	}

	/** 把资产的当前状态读成计划用的节点（只有属性，没有子树）。 */
	void ReadCurrentNode(const UBlueprint* InBlueprint, FForgeNode& OutRoot, TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		OutRoot.ClassName = ObjectRootClass;
		OutRoot.Name = TEXT("ClassDefaults");

		const UObject* ClassDefaultObject = ResolveClassDefaultObject(InBlueprint);
		if (ClassDefaultObject == nullptr || InBlueprint->ParentClass == nullptr)
		{
			return;
		}

		// 默认基准必须是**父类**的 CDO：生成类 CDO 上的值就是"当前值"本身，拿它当基准等于与
		// 自己比较，导出恒为空、配置永远不收敛。这与导入侧的 PruneDefaults(ParentClass) 同基准。
		const UObject* ParentDefaultObject = InBlueprint->ParentClass->GetDefaultObject();

		ForgePropertyCodec::ExportStructDelta(
			InBlueprint->ParentClass,
			ClassDefaultObject,
			ParentDefaultObject,
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

	if (Blueprint->ParentClass == nullptr || Blueprint->ParentClass->IsChildOf(AActor::StaticClass()))
	{
		OutReport.AddError(
			TEXT("parent"),
			FString::Printf(
				TEXT("%s 的父类是 AActor 派生——它属于 actor-blueprint（组件树那一侧）"),
				*InAssetPath));
		return false;
	}

	OutDocument.Kind = Kind;
	OutDocument.SchemaVersion = ForgeDoc::CurrentSchemaVersion;
	OutDocument.Asset = InAssetPath;
	OutDocument.Parent = Blueprint->ParentClass->GetPathName();

	ReadCurrentNode(Blueprint, OutDocument.Root, OutReport.Diagnostics);

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

	// 这里没有组件树：写了 children 就是配置错误——静默忽略会让作者以为它生效了。
	if (InDocument.Root.Children.Num() > 0)
	{
		OutReport.AddError(
			TEXT("root.children"),
			TEXT("本类型没有组件树：children 必须为空（Actor 类蓝图请用 actor-blueprint）"));
		return false;
	}

	ForgePropertyCodec::ValidateProperties(
		ParentClass,
		InDocument.Root.Props,
		OutReport.Diagnostics,
		TEXT("root.props"));
	ForgePropertyCodec::PruneDefaults(ParentClass, InDocument.Root.Props);

	UBlueprint* Blueprint = FindBlueprint(InDocument.Asset);
	bool bCreatedAsset = false;

	// 计划：按属性名做增删改（与 data-asset 同一套——这里属性既是身份也是内容）。
	FForgeNode CurrentRoot;
	ReadCurrentNode(Blueprint, CurrentRoot, OutReport.Diagnostics);

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
				TEXT("资产路径 `%s` 不合法，应形如 /Game/GAS/Ability/GA_Fire"), *InDocument.Asset));
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

		// 新建的资产即使一份属性都不改也要落盘——"配置为空"不等于"不用建资产"（例如全用默认值的技能）。
		bCreatedAsset = true;
	}
	else if (Blueprint->ParentClass != ParentClass)
	{
		// 换父类会让已有默认值失去解释（属性可能不再存在），不能悄悄做。
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

	// 先让生成类就位（新建的与要改的都先编译一次），再写它的默认对象：
	// 编译会重建 GeneratedClass 的 CDO——写进"旧 CDO"的 Instanced 子对象（GE 组件、
	// 输入修饰器这类数组）会随重建变成出包的引用，保存时被静默丢弃
	//（2026-10-07 实测：GE_Cooldown_Primary 的授予标签配置就这么丢的）。
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	UObject* ClassDefaultObject = ResolveClassDefaultObject(Blueprint);
	if (ClassDefaultObject == nullptr)
	{
		OutReport.AddError(TEXT("asset"), TEXT("蓝图没有可写的默认对象（GeneratedClass 与 ParentClass 都不可用）"));
		return false;
	}

	// 配置是唯一真源：先把属性重置到**父类默认值**（配置写的是相对父类的增量），再写配置声明的属性。
	ForgePropertyCodec::ResetStructToDefaults(Blueprint->ParentClass, ClassDefaultObject);
	ForgePropertyCodec::ImportStructProperties(
		Blueprint->ParentClass,
		ClassDefaultObject,
		InDocument.Root.Props,
		OutReport.Diagnostics,
		TEXT("root.props"),
		ClassDefaultObject);

	if (OutReport.HasErrors())
	{
		return false;
	}

	// 写完后**不再编译**：再编译会把刚写进的 Instanced 子对象又一次吹掉。
	Blueprint->MarkPackageDirty();

	FString SaveError;
	if (!SaveAssetPackage(Blueprint, SaveError))
	{
		OutReport.AddError(TEXT("asset"), SaveError);
		return false;
	}

	OutReport.bSucceeded = true;
	return true;
}

} // namespace ObjectBlueprintForge
