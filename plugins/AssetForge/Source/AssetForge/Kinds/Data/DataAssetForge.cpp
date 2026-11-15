// Copyright Epic Games, Inc. All Rights Reserved.

#include "Kinds/Data/DataAssetForge.h"
#include "Core/ForgeDiff.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgePropertyCodec.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/DataAsset.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace DataAssetForge
{

namespace
{
	UDataAsset* FindDataAsset(const FString& InAssetPath)
	{
		if (InAssetPath.IsEmpty())
		{
			return nullptr;
		}

		return Cast<UDataAsset>(LoadObject<UObject>(nullptr, *InAssetPath));
	}

	UClass* ResolveAssetClass(const FString& InClassName, FString& OutError)
	{
		if (InClassName.IsEmpty())
		{
			OutError = TEXT("缺少 root.class：数据资产必须指明自己是哪个类的实例");
			return nullptr;
		}

		UClass* Class = LoadClass<UObject>(nullptr, *InClassName);
		if (Class == nullptr)
		{
			OutError = FString::Printf(TEXT("找不到资产类 `%s`"), *InClassName);
			return nullptr;
		}

		if (!Class->IsChildOf(UDataAsset::StaticClass()))
		{
			OutError = FString::Printf(
				TEXT("`%s` 不是 UDataAsset 的派生类；其它类型的资产各有自己的 kind"),
				*InClassName);
			return nullptr;
		}

		// 抽象类建不出实例，而引擎只在保存时警告一句——必须在这里拦住。
		if (Class->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = FString::Printf(
				TEXT("`%s` 是抽象类，建不出资产；请写它某个非抽象的派生类"),
				*InClassName);
			return nullptr;
		}

		return Class;
	}

	bool SaveAssetPackage(UObject* InAsset, FString& OutError)
	{
		UPackage* Package = InAsset->GetOutermost();
		Package->MarkPackageDirty();

		// 与 UMG 那边同理：新建的包在第一次保存前 FileName 是空的，必须自己算出磁盘路径。
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

	/** 把一份资产读成节点（用于与配置比对）。 */
	void ReadAssetInto(UDataAsset* InAsset, FForgeNode& OutNode, TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		OutNode.ClassName = InAsset->GetClass()->GetPathName();
		OutNode.Name = InAsset->GetName();

		ForgePropertyCodec::ExportDeltaProperties(
			InAsset,
			OutNode.Props,
			OutDiagnostics,
			TEXT("root.props"));
	}
}

bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport)
{
	OutReport.AssetPath = InAssetPath;

	UDataAsset* Asset = FindDataAsset(InAssetPath);
	if (Asset == nullptr)
	{
		OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("找不到数据资产：%s"), *InAssetPath));
		return false;
	}

	OutDocument.Kind = Kind;
	OutDocument.SchemaVersion = ForgeDoc::CurrentSchemaVersion;
	OutDocument.Asset = InAssetPath;
	OutDocument.Parent = Asset->GetClass()->GetPathName();

	ReadAssetInto(Asset, OutDocument.Root, OutReport.Diagnostics);

	OutReport.bSucceeded = true;
	return true;
}

bool ImportDocument(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport)
{
	OutReport.bDryRun = bDryRun;
	OutReport.SourcePath = InDocument.SourcePath;
	OutReport.AssetPath = InDocument.Asset;

	FString ClassError;
	UClass* AssetClass = ResolveAssetClass(InDocument.Root.ClassName, ClassError);
	if (AssetClass == nullptr)
	{
		OutReport.AddError(TEXT("root.class"), ClassError);
		return false;
	}

	// 先校验再归一化：校验要看的是作者写了什么，归一化改的是拿去比较的形态。
	ForgePropertyCodec::ValidateProperties(
		AssetClass,
		InDocument.Root.Props,
		OutReport.Diagnostics,
		TEXT("root.props"));
	ForgePropertyCodec::PruneDefaults(AssetClass, InDocument.Root.Props);

	UDataAsset* Asset = FindDataAsset(InDocument.Asset);
	if (InDocument.Root.Children.Num() > 0)
	{
		OutReport.AddError(
			TEXT("root.children"),
			TEXT("数据资产没有子节点；属性直接写在 root.props 里"));
	}

	// 计划：新资产是全量创建，已有资产按属性差异算更新。
	if (Asset == nullptr)
	{
		OutReport.Actions.Add({ EForgeAction::Create, TEXT("root"), InDocument.Root.Name, FString() });
	}
	else
	{
		FForgeNode CurrentNode;
		ReadAssetInto(Asset, CurrentNode, OutReport.Diagnostics);
		ForgeDiff::ComputeRootPlan(InDocument.Root, CurrentNode, OutReport.Actions);
	}

	OutReport.CountActions();

	if (bDryRun)
	{
		OutReport.bSucceeded = !OutReport.HasErrors();
		return OutReport.bSucceeded;
	}

	// 带着错误去改资产会留下改到一半的状态。
	if (OutReport.HasErrors())
	{
		return false;
	}

	if (Asset == nullptr)
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
				TEXT("资产路径 `%s` 不合法，应形如 /Game/Asset/Atmosphere/DA_Xxx"), *InDocument.Asset));
			return false;
		}

		Asset = NewObject<UDataAsset>(Package, AssetClass, FName(*AssetName), RF_Public | RF_Standalone);
		if (Asset == nullptr)
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("创建资产失败：%s"), *InDocument.Asset));
			return false;
		}

		FAssetRegistryModule::AssetCreated(Asset);
		Package->MarkPackageDirty();
	}

	// 没有动作就不要写盘：无意义的保存会改时间戳、触发下游重导，也让「改动」这件事失真。
	if (OutReport.Actions.Num() == 0)
	{
		OutReport.bSucceeded = true;
		return true;
	}

	// 先回默认再应用配置：配置没写的属性不该在资产里残留旧值。
	ForgePropertyCodec::ResetToDefaults(Asset);
	ForgePropertyCodec::ImportProperties(
		Asset,
		InDocument.Root.Props,
		OutReport.Diagnostics,
		TEXT("root.props"));

	if (OutReport.HasErrors())
	{
		return false;
	}

	FString SaveError;
	if (!SaveAssetPackage(Asset, SaveError))
	{
		OutReport.AddError(TEXT("asset"), SaveError);
		return false;
	}

	OutReport.bSucceeded = true;
	return true;
}

} // namespace DataAssetForge
