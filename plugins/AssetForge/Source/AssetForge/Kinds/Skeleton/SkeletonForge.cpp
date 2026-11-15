// Copyright Epic Games, Inc. All Rights Reserved.

#include "Kinds/Skeleton/SkeletonForge.h"

#include "Core/ForgeDiff.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgePropertyCodec.h"

#include "Misc/PackageName.h"
#include "Animation/Skeleton.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace SkeletonForge
{

namespace
{
	/** 纳管型：资产必须已存在（先由 引擎工具做出来）。 */
	USkeleton* FindSkeleton(const FString& InAssetPath)
	{
		if (InAssetPath.IsEmpty())
		{
			return nullptr;
		}

		return Cast<USkeleton>(LoadObject<UObject>(nullptr, *InAssetPath));
	}

	bool SaveAssetPackage(UObject* InAsset, FString& OutError)
	{
		UPackage* Package = InAsset->GetOutermost();
		Package->MarkPackageDirty();

		const FString PackageFileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;

		if (!UPackage::SavePackage(Package, InAsset, *PackageFileName, SaveArgs))
		{
			OutError = FString::Printf(TEXT("保存失败：%s"), *PackageFileName);
			return false;
		}

		return true;
	}
}

bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport)
{
	OutReport.AssetPath = InAssetPath;

	USkeleton* Skeleton = FindSkeleton(InAssetPath);
	if (Skeleton == nullptr)
	{
		OutReport.AddError(
			TEXT("asset"),
			FString::Printf(
				TEXT("找不到骨骼资产：%s（纳管型 kind 只微调已存在的资产——先用 引擎工具做出来）"),
				*InAssetPath));
		return false;
	}

	OutDocument.Kind = Kind;
	OutDocument.SchemaVersion = ForgeDoc::CurrentSchemaVersion;
	OutDocument.Asset = InAssetPath;
	OutDocument.Parent.Reset();
	OutDocument.Root.ClassName = Skeleton->GetClass()->GetName();
	OutDocument.Root.Name = Skeleton->GetName();

	// 全量导出（InDefaultData = nullptr）：纳管型的配置要能整份手改再导回。
	ForgePropertyCodec::ExportStructDelta(
		Skeleton->GetClass(),
		Skeleton,
		/*InDefaultData=*/nullptr,
		OutDocument.Root.Props,
		OutReport.Diagnostics,
		TEXT("root.props"));

	OutReport.bSucceeded = true;
	return true;
}

bool ImportDocument(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport)
{
	OutReport.bDryRun = bDryRun;
	OutReport.SourcePath = InDocument.SourcePath;
	OutReport.AssetPath = InDocument.Asset;

	USkeleton* Skeleton = FindSkeleton(InDocument.Asset);
	if (Skeleton == nullptr)
	{
		OutReport.AddError(
			TEXT("asset"),
			FString::Printf(
				TEXT("找不到骨骼资产：%s（纳管型 kind 只微调已存在的资产）"),
				*InDocument.Asset));
		return false;
	}

	// 校验先跑（动资产之前）。
	ForgePropertyCodec::ValidateProperties(Skeleton->GetClass(), InDocument.Root.Props, OutReport.Diagnostics, TEXT("root.props"));
	if (OutReport.HasErrors())
	{
		return false;
	}

	// 计划：当前值全量读出来做对比。
	FForgeNode CurrentRoot;
	CurrentRoot.ClassName = Skeleton->GetClass()->GetName();
	CurrentRoot.Name = Skeleton->GetName();
	ForgePropertyCodec::ExportStructDelta(
		Skeleton->GetClass(),
		Skeleton,
		/*InDefaultData=*/nullptr,
		CurrentRoot.Props,
		OutReport.Diagnostics,
		TEXT("root.props"));

	ForgeDiff::ComputeRootPlan(InDocument.Root, CurrentRoot, OutReport.Actions);
	OutReport.CountActions();

	if (bDryRun)
	{
		OutReport.bSucceeded = !OutReport.HasErrors();
		return OutReport.bSucceeded;
	}

	// 无变更不写盘（幂等）。
	if (OutReport.Actions.Num() == 0)
	{
		OutReport.bSucceeded = true;
		return true;
	}

	// 微调语义：只写配置里出现的字段。
	ForgePropertyCodec::ImportProperties(Skeleton, InDocument.Root.Props, OutReport.Diagnostics, TEXT("root.props"));
	if (OutReport.HasErrors())
	{
		return false;
	}

	FString SaveError;
	if (!SaveAssetPackage(Skeleton, SaveError))
	{
		OutReport.AddError(TEXT("asset"), SaveError);
		return false;
	}

	OutReport.bSucceeded = true;
	return true;
}

}
