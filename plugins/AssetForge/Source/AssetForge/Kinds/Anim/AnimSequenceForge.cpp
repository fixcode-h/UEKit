// Copyright Epic Games, Inc. All Rights Reserved.

#include "Kinds/Anim/AnimSequenceForge.h"

#include "Core/ForgeDiff.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgePropertyCodec.h"

#include "Animation/AnimSequenceBase.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace AnimSequenceForge
{

namespace
{
	/** 纳管型：资产必须已存在（导入端不做生成）。 */
	UAnimSequenceBase* FindAnimAsset(const FString& InAssetPath)
	{
		if (InAssetPath.IsEmpty())
		{
			return nullptr;
		}

		return Cast<UAnimSequenceBase>(LoadObject<UObject>(nullptr, *InAssetPath));
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

	UAnimSequenceBase* Anim = FindAnimAsset(InAssetPath);
	if (Anim == nullptr)
	{
		OutReport.AddError(
			TEXT("asset"),
			FString::Printf(
				TEXT("找不到动画资产：%s（纳管型 kind 只微调已存在的资产——先用 FBX 导入或引擎工具做出来）"),
				*InAssetPath));
		return false;
	}

	OutDocument.Kind = Kind;
	OutDocument.SchemaVersion = ForgeDoc::CurrentSchemaVersion;
	OutDocument.Asset = InAssetPath;
	OutDocument.Parent.Reset();
	OutDocument.Root.ClassName = Anim->GetClass()->GetName();
	OutDocument.Root.Name = Anim->GetName();

	// 全量导出（InDefaultData = nullptr）：纳管型的配置要能整份手改再导回，
	// 不能像生成型那样只留 delta——否则"配置没写"与"等于默认"分不开。
	ForgePropertyCodec::ExportStructDelta(
		Anim->GetClass(),
		Anim,
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

	UAnimSequenceBase* Anim = FindAnimAsset(InDocument.Asset);
	if (Anim == nullptr)
	{
		OutReport.AddError(
			TEXT("asset"),
			FString::Printf(
				TEXT("找不到动画资产：%s（纳管型 kind 只微调已存在的资产）"),
				*InDocument.Asset));
		return false;
	}

	// 校验先跑（动资产之前）：字段名写错时留下的是完整资产，不是改到一半的状态。
	ForgePropertyCodec::ValidateProperties(Anim->GetClass(), InDocument.Root.Props, OutReport.Diagnostics, TEXT("root.props"));
	if (OutReport.HasErrors())
	{
		return false;
	}

	// 计划（dry-run 与报告）：把当前值全量读出来做对比。
	FForgeNode CurrentRoot;
	CurrentRoot.ClassName = Anim->GetClass()->GetName();
	CurrentRoot.Name = Anim->GetName();
	ForgePropertyCodec::ExportStructDelta(
		Anim->GetClass(),
		Anim,
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

	// 无变更不写盘（幂等：导出的配置原样导回时，资产一个字节都不该动）。
	if (OutReport.Actions.Num() == 0)
	{
		OutReport.bSucceeded = true;
		return true;
	}

	// 微调语义：**只写配置里出现的字段**——不重置、不剪枝，配置没写的保持资产原值。
	// 数组字段（Notifies/Sections）是整体替换：增删就是改数组，由通用编解码承担。
	ForgePropertyCodec::ImportProperties(Anim, InDocument.Root.Props, OutReport.Diagnostics, TEXT("root.props"));
	if (OutReport.HasErrors())
	{
		return false;
	}

	FString SaveError;
	if (!SaveAssetPackage(Anim, SaveError))
	{
		OutReport.AddError(TEXT("asset"), SaveError);
		return false;
	}

	OutReport.bSucceeded = true;
	return true;
}

}
