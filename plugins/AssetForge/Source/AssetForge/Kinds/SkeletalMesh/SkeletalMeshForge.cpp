// Copyright Epic Games, Inc. All Rights Reserved.

#include "Kinds/SkeletalMesh/SkeletalMeshForge.h"

#include "Core/ForgeDiff.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgePropertyCodec.h"

#include "Misc/PackageName.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace SkeletalMeshForge
{

namespace
{
	/** 纳管型：资产必须已存在（先由 引擎工具做出来）。 */
	USkeletalMesh* FindPhysicsAsset(const FString& InAssetPath)
	{
		if (InAssetPath.IsEmpty())
		{
			return nullptr;
		}

		return Cast<USkeletalMesh>(LoadObject<UObject>(nullptr, *InAssetPath));
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

	USkeletalMesh* PhysicsAsset = FindPhysicsAsset(InAssetPath);
	if (PhysicsAsset == nullptr)
	{
		OutReport.AddError(
			TEXT("asset"),
			FString::Printf(
				TEXT("找不到骨骼网格体：%s（纳管型 kind 只微调已存在的资产——先用 FBX 导入或引擎工具做出来）"),
				*InAssetPath));
		return false;
	}

	OutDocument.Kind = Kind;
	OutDocument.SchemaVersion = ForgeDoc::CurrentSchemaVersion;
	OutDocument.Asset = InAssetPath;
	OutDocument.Parent.Reset();
	OutDocument.Root.ClassName = PhysicsAsset->GetClass()->GetName();
	OutDocument.Root.Name = PhysicsAsset->GetName();

	// 全量导出（InDefaultData = nullptr）：纳管型的配置要能整份手改再导回。
	ForgePropertyCodec::ExportStructDelta(
		PhysicsAsset->GetClass(),
		PhysicsAsset,
		/*InDefaultData=*/nullptr,
		OutDocument.Root.Props,
		OutReport.Diagnostics,
		TEXT("root.props"));

	// SourceModels 是导入派生数据（LOD 原始网格，含内嵌 bulk data 对象引用）：进配置只会
	// 制造噪音，而且导入端处理不了内嵌引用（会丢成 None → 「导出 → 导入」不收敛，实测）。
	if (OutDocument.Root.Props.IsValid())
	{
		OutDocument.Root.Props->RemoveField(TEXT("SourceModels"));
	}

	OutReport.bSucceeded = true;
	return true;
}

bool ImportDocument(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport)
{
	OutReport.bDryRun = bDryRun;
	OutReport.SourcePath = InDocument.SourcePath;
	OutReport.AssetPath = InDocument.Asset;

	USkeletalMesh* PhysicsAsset = FindPhysicsAsset(InDocument.Asset);
	if (PhysicsAsset == nullptr)
	{
		OutReport.AddError(
			TEXT("asset"),
			FString::Printf(
				TEXT("找不到骨骼网格体：%s（纳管型 kind 只微调已存在的资产）"),
				*InDocument.Asset));
		return false;
	}

	// 校验先跑（动资产之前）。
	ForgePropertyCodec::ValidateProperties(PhysicsAsset->GetClass(), InDocument.Root.Props, OutReport.Diagnostics, TEXT("root.props"));
	if (OutReport.HasErrors())
	{
		return false;
	}

	// 计划：当前值全量读出来做对比。
	FForgeNode CurrentRoot;
	CurrentRoot.ClassName = PhysicsAsset->GetClass()->GetName();
	CurrentRoot.Name = PhysicsAsset->GetName();
	ForgePropertyCodec::ExportStructDelta(
		PhysicsAsset->GetClass(),
		PhysicsAsset,
		/*InDefaultData=*/nullptr,
		CurrentRoot.Props,
		OutReport.Diagnostics,
		TEXT("root.props"));

	// 与导出侧对齐：读当前值时同样排除 SourceModels，否则每次导入都会算出
	// 「配置里没有、资产里有」的差异，幂等永远不成立（实测）。
	if (CurrentRoot.Props.IsValid())
	{
		CurrentRoot.Props->RemoveField(TEXT("SourceModels"));
	}

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
	ForgePropertyCodec::ImportProperties(PhysicsAsset, InDocument.Root.Props, OutReport.Diagnostics, TEXT("root.props"));
	if (OutReport.HasErrors())
	{
		return false;
	}

	FString SaveError;
	if (!SaveAssetPackage(PhysicsAsset, SaveError))
	{
		OutReport.AddError(TEXT("asset"), SaveError);
		return false;
	}

	OutReport.bSucceeded = true;
	return true;
}

}
