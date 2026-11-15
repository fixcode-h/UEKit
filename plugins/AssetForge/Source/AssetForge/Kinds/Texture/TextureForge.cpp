// Copyright Epic Games, Inc. All Rights Reserved.

#include "Kinds/Texture/TextureForge.h"

#include "Core/ForgeDiff.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgePaths.h"
#include "Core/ForgePropertyCodec.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "ImageUtils.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace TextureForge
{

namespace
{
	/** 配置里 root.source 的字段名——它住在节点的扩展字段里（管道不认识，原样保留）。 */
	const TCHAR* const SourceField = TEXT("source");

	/**
	 * 关键参数：导出时显式写出来（哪怕等于默认），归一化时也不许删。
	 *
	 * 这些是纹理的"一眼设置"——压缩组、寻址、mip 生成、尺寸上限。藏进 delta 省略里，
	 * 配置的读者得回引擎默认里猜；写全了，看一眼 JSON 就知道这张图会怎么进包。
	 */
	const TArray<FName>& KeyPropertyNames()
	{
		static const TArray<FName> Names = {
			TEXT("SRGB"),
			TEXT("CompressionSettings"),
			TEXT("LODGroup"),
			TEXT("MipGenSettings"),
			TEXT("MaxTextureSize"),
			TEXT("Filter"),
			TEXT("AddressX"),
			TEXT("AddressY"),
			TEXT("NeverStream"),
			TEXT("CompressionNoAlpha"),
		};
		return Names;
	}

	/** root.class 的短名写法；解析时补上脚本前缀。 */
	const TCHAR* const Texture2DClassShort = TEXT("Texture2D");

	/** 短类名补全用的脚本前缀。 */
	const TCHAR* const EngineScriptPrefix = TEXT("/Script/Engine.");

	/** 目标资产的尾名（新建时的对象名）。 */
	FString AssetNameOf(const FString& InAssetPath)
	{
		FString PackagePath;
		FString AssetName;
		InAssetPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		return AssetName;
	}

	/** 解析配置声明的纹理类；只接受 `UTexture2D` 家族（Cube/Volume/2DArray 不归这里管）。 */
	UClass* ResolveTextureClass(const FString& InClassName, FString& OutError)
	{
		FString FullName = InClassName;
		if (!FullName.StartsWith(TEXT("/")))
		{
			FullName = FString(EngineScriptPrefix) + FullName;
		}

		UClass* Class = LoadClass<UObject>(nullptr, *FullName);
		if (Class == nullptr)
		{
			OutError = FString::Printf(TEXT("找不到纹理类 `%s`"), *InClassName);
			return nullptr;
		}

		if (!Class->IsChildOf(UTexture2D::StaticClass()))
		{
			OutError = FString::Printf(
				TEXT("`%s` 不是 UTexture2D 的派生类；本处理器只支持 2D 纹理"),
				*InClassName);
			return nullptr;
		}

		return Class;
	}

	/**
	 * 资产的导入记录里的源文件绝对路径；没有记录返回空。
	 *
	 * 导入记录里存的是**相对项目目录**的路径（`UAssetImportData::Update` 的行为），
	 * 所以相对形态要按项目根折算，不能拿当前工作目录去 Convert。
	 */
	FString ReadImportSourcePath(const UTexture2D* InTexture)
	{
		// `FSourceFile::RelativeFilename` 的官方语义是"相对资产包、相对 BaseDir()、或绝对"——
		// 三者都可能，且写它的 `UAssetImportData::Update` 还会做一次相对化。这里把几种解释
		// 都试一遍、取真实存在的那个；都不存在时返回最可能的一种（上层报错时能看到路径长什么样）。
		if (InTexture->AssetImportData == nullptr)
		{
			return FString();
		}

		const TArray<FAssetImportInfo::FSourceFile>& SourceFiles = InTexture->AssetImportData->SourceData.SourceFiles;
		if (SourceFiles.Num() == 0 || SourceFiles[0].RelativeFilename.IsEmpty())
		{
			return FString();
		}

		const FString& Stored = SourceFiles[0].RelativeFilename;

		TArray<FString> Candidates;
		if (FPaths::IsRelative(Stored))
		{
			Candidates.Add(ForgePaths::CollapseToAbsolute(FPaths::Combine(FPaths::ProjectDir(), Stored)));
			Candidates.Add(ForgePaths::CollapseToAbsolute(FPaths::Combine(FPlatformProcess::BaseDir(), Stored)));
		}
		Candidates.Add(ForgePaths::CollapseToAbsolute(Stored));

		for (const FString& Candidate : Candidates)
		{
			if (FPaths::FileExists(Candidate))
			{
				return Candidate;
			}
		}

		return Candidates[0];
	}

	/** 导入记录里源文件的时间戳；没有记录返回 MinValue。 */
	FDateTime ReadImportSourceTimestamp(const UTexture2D* InTexture)
	{
		if (InTexture->AssetImportData != nullptr)
		{
			const TArray<FAssetImportInfo::FSourceFile>& SourceFiles = InTexture->AssetImportData->SourceData.SourceFiles;
			if (SourceFiles.Num() > 0)
			{
				return SourceFiles[0].Timestamp;
			}
		}

		return FDateTime::MinValue();
	}

	/** `ERawImageFormat` → 纹理源格式。覆盖常见解码结果（8/16 位与浮点）。 */
	ETextureSourceFormat ToTextureSourceFormat(ERawImageFormat::Type InFormat)
	{
		switch (InFormat)
		{
		case ERawImageFormat::G8:
			return TSF_G8;
		case ERawImageFormat::G16:
			return TSF_G16;
		case ERawImageFormat::BGRA8:
			return TSF_BGRA8;
		case ERawImageFormat::BGRE8:
			return TSF_BGRE8;
		case ERawImageFormat::RGBA16:
			return TSF_RGBA16;
		case ERawImageFormat::RGBA16F:
			return TSF_RGBA16F;
		case ERawImageFormat::RGBA32F:
			return TSF_RGBA32F;
		case ERawImageFormat::R16F:
			return TSF_R16F;
		case ERawImageFormat::R32F:
			return TSF_R32F;
		default:
			return TSF_BGRA8;
		}
	}

	/** 把源文件的像素灌进纹理（重建 Source 数据）；失败写 OutError。 */
	bool LoadSourceIntoTexture(UTexture2D* InTexture, const FString& InAbsoluteSource, FString& OutError)
	{
		TArray<uint8> FileData;
		if (!FFileHelper::LoadFileToArray(FileData, *InAbsoluteSource))
		{
			OutError = FString::Printf(TEXT("读不到源图文件：%s"), *InAbsoluteSource);
			return false;
		}

		FImage Image;
		if (!FImageUtils::DecompressImage(FileData.GetData(), FileData.Num(), Image))
		{
			OutError = FString::Printf(
				TEXT("解不开源图：%s（支持 PNG/TGA/JPG/BMP/EXR/HDR 等常见格式）"),
				*InAbsoluteSource);
			return false;
		}

		// FImage 是单 mip 的位图；mip 链由引擎按纹理设置重建。
		InTexture->Source.Init(
			Image.SizeX,
			Image.SizeY,
			Image.NumSlices,
			/*InNumMips=*/1,
			ToTextureSourceFormat(Image.Format),
			Image.RawData.GetData());

		InTexture->UpdateResource();
		return true;
	}

	/** 把纹理读成节点（导出方向；导入的比对也读它）。 */
	void ReadTextureInto(const UTexture2D* InTexture, FForgeNode& OutNode, TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		OutNode.ClassName = Texture2DClassShort;
		OutNode.Name = InTexture->GetName();
		ForgePropertyCodec::ExportDeltaProperties(InTexture, OutNode.Props, OutDiagnostics, TEXT("root.props"));

		// 关键参数强制写出来（哪怕等于默认）：配置的读者要一眼看到压缩/寻址/mip 这些设置。
		ForgePropertyCodec::ExportForcedProperties(
			InTexture, KeyPropertyNames(), OutNode.Props, OutDiagnostics, TEXT("root.props"));
	}

	/** 配置里声明的源图（绝对路径）；没写返回空。 */
	FString ReadConfiguredSource(const FForgeNode& InRoot)
	{
		if (!InRoot.Extra.IsValid())
		{
			return FString();
		}

		FString Source;
		if (!InRoot.Extra->TryGetStringField(SourceField, Source) || Source.IsEmpty())
		{
			return FString();
		}

		return ForgePaths::CollapseToAbsolute(Source);
	}

	bool SaveAssetPackage(UObject* InAsset, FForgeReport& OutReport)
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
			OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("保存资产失败：%s"), *PackageFileName));
			return false;
		}

		return true;
	}
}

bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport)
{
	OutReport.AssetPath = InAssetPath;

	UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *InAssetPath);
	if (Texture == nullptr)
	{
		OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("找不到 2D 纹理资产：%s"), *InAssetPath));
		return false;
	}

	OutDocument.Kind = Kind;
	OutDocument.SchemaVersion = ForgeDoc::CurrentSchemaVersion;
	OutDocument.Asset = InAssetPath;

	ReadTextureInto(Texture, OutDocument.Root, OutReport.Diagnostics);

	const FString SourcePath = ReadImportSourcePath(Texture);
	if (SourcePath.IsEmpty())
	{
		OutReport.AddWarning(
			TEXT("root.source"),
			TEXT("资产没有导入源记录（不是从图片导入的）；配置里不会写 source，导入时也就不能重建像素"));
	}
	else
	{
		if (!OutDocument.Root.Extra.IsValid())
		{
			OutDocument.Root.Extra = MakeShared<FJsonObject>();
		}
		OutDocument.Root.Extra->SetStringField(SourceField, ForgePaths::MakeProjectRelative(SourcePath));
	}

	OutReport.bSucceeded = true;
	return true;
}

bool ImportDocument(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport)
{
	OutReport.bDryRun = bDryRun;
	OutReport.SourcePath = InDocument.SourcePath;
	OutReport.AssetPath = InDocument.Asset;

	// ---- 校验 ----

	if (InDocument.Root.Children.Num() > 0)
	{
		OutReport.AddError(
			TEXT("root.children"),
			TEXT("纹理配置没有子节点：内容来自 source（源图），设置写在 root.props"));
	}

	FString ClassError;
	UClass* TextureClass = ResolveTextureClass(InDocument.Root.ClassName, ClassError);
	if (TextureClass == nullptr)
	{
		OutReport.AddError(TEXT("root.class"), ClassError);
		return false;
	}

	const FString ConfiguredSource = ReadConfiguredSource(InDocument.Root);

	UTexture2D* Texture = nullptr;
	const bool bAssetExists = FPackageName::DoesPackageExist(InDocument.Asset);
	if (bAssetExists)
	{
		Texture = LoadObject<UTexture2D>(nullptr, *InDocument.Asset);
		if (Texture == nullptr)
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(
				TEXT("`%s` 存在但不是 2D 纹理；texture 配置的目标只能是 Texture2D"), *InDocument.Asset));
			return false;
		}

		// 目标资产的类由它自己的创建方式决定；配置写得对不上时明确报错，而不是按配置去换类
		// （换类会丢掉引用与导入设置，那是编辑器里的操作）。
		if (Texture->GetClass() != TextureClass)
		{
			OutReport.AddError(
				TEXT("root.class"),
				FString::Printf(
					TEXT("资产 `%s` 的类是 `%s`，与配置的 `%s` 不一致"),
					*InDocument.Asset,
					*Texture->GetClass()->GetPathName(),
					*InDocument.Root.ClassName));
			return false;
		}
	}

	// 源图：新资产必须写；写了的必须真的在磁盘上。
	if (!bAssetExists && ConfiguredSource.IsEmpty())
	{
		OutReport.AddError(TEXT("root.source"), TEXT("新纹理必须写 source：2D 纹理的内容来自源图"));
	}
	else if (!ConfiguredSource.IsEmpty() && !FPaths::FileExists(ConfiguredSource))
	{
		OutReport.AddError(TEXT("root.source"), FString::Printf(TEXT("找不到源图：%s"), *ConfiguredSource));
	}

	// 先校验再归一化：校验要看的是作者写了什么，归一化改的是拿去比较的形态。
	ForgePropertyCodec::ValidateProperties(
		TextureClass, InDocument.Root.Props, OutReport.Diagnostics, TEXT("root.props"));

	// 关键参数应当显式定义：导出方向永远写全，这条只会在手写配置漏项时命中——提醒而不阻断。
	{
		TArray<FString> MissingKeys;
		for (const FName& KeyName : KeyPropertyNames())
		{
			if (!InDocument.Root.Props.IsValid() || !InDocument.Root.Props->HasField(KeyName.ToString()))
			{
				MissingKeys.Add(KeyName.ToString());
			}
		}

		if (MissingKeys.Num() > 0)
		{
			OutReport.AddWarning(TEXT("root.props"), FString::Printf(
				TEXT("关键参数没有显式定义：%s（没定义就用引擎默认/资产现值；建议补全）"),
				*FString::Join(MissingKeys, TEXT("、"))));
		}
	}

	// 归一化：PruneDefaults 会删「等于默认」的冗余字段（让 diff 稳定），但关键参数要留着——
	// 先摘出来、Prune 完放回去。
	TMap<FString, TSharedPtr<FJsonValue>> PreservedKeyValues;
	if (InDocument.Root.Props.IsValid())
	{
		for (const FName& KeyName : KeyPropertyNames())
		{
			const FString KeyString = KeyName.ToString();
			if (InDocument.Root.Props->HasField(KeyString))
			{
				PreservedKeyValues.Add(KeyString, InDocument.Root.Props->TryGetField(KeyString));
			}
		}
	}

	ForgePropertyCodec::PruneDefaults(TextureClass, InDocument.Root.Props);

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : PreservedKeyValues)
	{
		if (!InDocument.Root.Props.IsValid())
		{
			InDocument.Root.Props = MakeShared<FJsonObject>();
		}
		InDocument.Root.Props->SetField(Pair.Key, Pair.Value);
	}

	// ---- 算计划 ----

	// 要不要重灌源图？三条里任一条成立就要：资产还不存在（新建）/ 源路径变了 / 源文件比上次导入新。
	// 它是像素内容，不参与属性 diff（那不是反射属性），所以由这里单独判、并进 root 的动作里。
	bool bReloadSource = false;
	FString ReloadReason;
	if (!ConfiguredSource.IsEmpty())
	{
		if (!bAssetExists)
		{
			bReloadSource = true;
			ReloadReason = TEXT("源图");
		}
		else
		{
			const FString CurrentSource = ReadImportSourcePath(Texture);
			if (CurrentSource != ConfiguredSource)
			{
				bReloadSource = true;
				ReloadReason = FString::Printf(
					TEXT("源图：%s → %s"),
					CurrentSource.IsEmpty() ? TEXT("（无记录）") : *ForgePaths::MakeProjectRelative(CurrentSource),
					*ForgePaths::MakeProjectRelative(ConfiguredSource));
			}
			else
			{
				const FDateTime ImportedAt = ReadImportSourceTimestamp(Texture);
				const FDateTime FileStamp = IFileManager::Get().GetTimeStamp(*ConfiguredSource);
				if (ImportedAt == FDateTime::MinValue() || FileStamp > ImportedAt + FTimespan::FromSeconds(1.0))
				{
					bReloadSource = true;
					ReloadReason = TEXT("源图已更新（文件比上次导入新）");
				}
			}
		}
	}

	if (!bAssetExists)
	{
		OutReport.Actions.Add({ EForgeAction::Create, TEXT("root"), InDocument.Root.Name, FString() });
	}
	else
	{
		FForgeNode CurrentNode;
		ReadTextureInto(Texture, CurrentNode, OutReport.Diagnostics);
		ForgeDiff::ComputeRootPlan(InDocument.Root, CurrentNode, OutReport.Actions);

		// 源图差异并进 root 的动作里：同一节点不该出现两条 Update。
		if (bReloadSource)
		{
			FForgePlanEntry* RootAction = nullptr;
			for (FForgePlanEntry& Entry : OutReport.Actions)
			{
				if (Entry.NodePath == TEXT("root") && Entry.Action == EForgeAction::Update)
				{
					RootAction = &Entry;
					break;
				}
			}

			if (RootAction != nullptr)
			{
				RootAction->Detail = RootAction->Detail.IsEmpty()
					? ReloadReason
					: FString::Printf(TEXT("%s；%s"), *RootAction->Detail, *ReloadReason);
			}
			else
			{
				OutReport.Actions.Add({ EForgeAction::Update, TEXT("root"), InDocument.Root.Name, ReloadReason });
			}
		}
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

	// 没有动作就不要写盘：无意义的保存会改时间戳、触发下游重导，也让"改动"这件事失真。
	if (OutReport.Actions.Num() == 0)
	{
		OutReport.bSucceeded = true;
		return true;
	}

	// ---- 执行 ----

	if (Texture == nullptr)
	{
		const FString AssetName = AssetNameOf(InDocument.Asset);
		if (AssetName.IsEmpty())
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(
				TEXT("资产路径 `%s` 不合法，应形如 /Game/.../T_Xxx"), *InDocument.Asset));
			return false;
		}

		UPackage* Package = CreatePackage(*InDocument.Asset);
		if (Package == nullptr)
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("无法创建包：%s"), *InDocument.Asset));
			return false;
		}

		Texture = NewObject<UTexture2D>(Package, FName(*AssetName), RF_Public | RF_Standalone);
		if (Texture == nullptr)
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("创建纹理失败：%s"), *InDocument.Asset));
			return false;
		}

		FAssetRegistryModule::AssetCreated(Texture);
	}

	if (bReloadSource)
	{
		FString SourceError;
		if (!LoadSourceIntoTexture(Texture, ConfiguredSource, SourceError))
		{
			OutReport.AddError(TEXT("root.source"), SourceError);
			return false;
		}

		if (Texture->AssetImportData == nullptr)
		{
			Texture->AssetImportData = NewObject<UAssetImportData>(Texture);
		}

		// 直接构造记录（绝对路径 + 源文件时间戳），不走 `UAssetImportData::Update`：
		// Update 会把路径相对化（基准可能是引擎目录），读回来时得再猜一遍；写绝对路径，
		// 读永远准——`ReadImportSourcePath` 的多候选只是给手工导入的老资产兜底。
		Texture->AssetImportData->SourceData.SourceFiles.Empty();
		Texture->AssetImportData->SourceData.SourceFiles.Add(FAssetImportInfo::FSourceFile(
			ConfiguredSource,
			IFileManager::Get().GetTimeStamp(*ConfiguredSource)));
	}

	// 先回默认再应用配置：配置没写的属性不该在资产里残留旧值。
	ForgePropertyCodec::ResetToDefaults(Texture);
	ForgePropertyCodec::ImportProperties(
		Texture, InDocument.Root.Props, OutReport.Diagnostics, TEXT("root.props"));

	if (OutReport.HasErrors())
	{
		return false;
	}

	// 设置变化（sRGB / 压缩 / Mip 生成……）要让引擎按新属性重烘焙派生数据。
	Texture->UpdateResource();

	if (!SaveAssetPackage(Texture, OutReport))
	{
		return false;
	}

	OutReport.bSucceeded = true;
	return true;
}

} // namespace TextureForge
