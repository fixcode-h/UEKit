// Copyright Epic Games, Inc. All Rights Reserved.

#include "Kinds/Font/FontForge.h"
#include "AssetForgeCompat.h"

#include "Core/ForgeDiff.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgePaths.h"
#include "Core/ForgePropertyCodec.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "Fonts/CompositeFont.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Rendering/SlateRenderer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace FontForge
{

namespace
{
	/** 配置里 root.children[i] 的扩展字段——它们不是资产属性，管道不认识、只负责保留。 */
	const TCHAR* const FaceSourceField = TEXT("source");
	const TCHAR* const FaceTypefaceField = TEXT("typeface");

	/** root.class / children[i].class 的短名写法；解析时补上脚本前缀。 */
	const TCHAR* const FontClassShort = TEXT("Font");
	const TCHAR* const FontFaceClassShort = TEXT("FontFace");
	const TCHAR* const EngineScriptPrefix = TEXT("/Script/Engine.");

	const TCHAR* const CompositeFontProperty = TEXT("CompositeFont");
	const TCHAR* const LegacyFontNameProperty = TEXT("LegacyFontName");
	const TCHAR* const SourceFilenameProperty = TEXT("SourceFilename");
	const TCHAR* const FontCacheTypeProperty = TEXT("FontCacheType");

	/** 关键参数：导出时显式写出来（哪怕等于默认），归一化时也不许删。 */
	const TArray<FName>& FontKeyPropertyNames()
	{
		static const TArray<FName> Names = { TEXT("FontCacheType") };
		return Names;
	}

	const TArray<FName>& FaceKeyPropertyNames()
	{
		static const TArray<FName> Names = { TEXT("Hinting"), TEXT("LoadingPolicy") };
		return Names;
	}

	/**
	 * 不能进配置的属性（导出按名单排除、导入明确拒绝，两边都守着）：
	 *
	 * - `UFont::CompositeFont`：字体结构由 children 表达。放它进配置等于给结构开第二个来源，
	 *   "配置里没有的会被删"这条契约就保不住（它内部的 FName 也过不了 JSON 通道）。
	 * - `UFont::LegacyFontName`：FName 标量，`FJsonObjectConverter` 不支持（导入必失败）。
	 * - `UFontFace::SourceFilename`：源文件由 `source` 字段表达，避免同一件事两处真相。
	 */
	const TArray<FName>& FontExcludedPropertyNames()
	{
		static const TArray<FName> Names = { FName(CompositeFontProperty), FName(LegacyFontNameProperty) };
		return Names;
	}

	const TArray<FName>& FaceExcludedPropertyNames()
	{
		static const TArray<FName> Names = { FName(SourceFilenameProperty) };
		return Names;
	}

	/** 属性名是否在排除名单里（导入方向用，措辞按名字给具体理由）。 */
	bool IsExcludedProperty(const TArray<FName>& InExcludedNames, const FString& InPropertyName)
	{
		for (const FName& Name : InExcludedNames)
		{
			if (Name.ToString() == InPropertyName)
			{
				return true;
			}
		}
		return false;
	}

	/** 配置里的一个字体面（children 的一项，解析并校验过）。 */
	struct FFaceSpec
	{
		/** FontFace 资产名；资产路径 = 与 UFont 同目录 + 该名字。 */
		FString AssetName;

		/** 在 UFont 里的 typeface 名（UI 里选的就是它）。 */
		FString TypefaceName;

		/** 配置里的原样写法（诊断文案用）。 */
		FString SourceConfig;

		/** 解析后的绝对路径。 */
		FString SourcePath;

		TSharedPtr<FJsonObject> Props;
	};

	/** 当前 UFont 的一个默认字体面：entry 名 + 它指向的 FontFace 资产（旧式裸文件名引用时为 null）。 */
	struct FCurrentFace
	{
		FName EntryName;
		const UFontFace* Face = nullptr;
	};

	FString AssetNameOf(const FString& InAssetPath)
	{
		FString PackagePath;
		FString AssetName;
		InAssetPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		return AssetName;
	}

	/** 字体面资产路径 = UFont 所在目录 + 面名（配置不单独表达路径，避免两个来源）。 */
	FString FaceAssetPathOf(const FString& InFontAssetPath, const FString& InFaceName)
	{
		return FString::Printf(TEXT("%s/%s"), *FPaths::GetPath(InFontAssetPath), *InFaceName);
	}

	/** 解析配置声明的类；短名按 /Script/Engine 展开，且必须是期望基类的派生。 */
	UClass* ResolveRequiredClass(
		const FString& InClassName,
		UClass* InRequiredBase,
		const TCHAR* InRequiredBaseName,
		FString& OutError)
	{
		FString FullName = InClassName;
		if (!FullName.StartsWith(TEXT("/")))
		{
			FullName = FString(EngineScriptPrefix) + FullName;
		}

		UClass* Class = LoadClass<UObject>(nullptr, *FullName);
		if (Class == nullptr)
		{
			OutError = FString::Printf(TEXT("找不到类 `%s`"), *InClassName);
			return nullptr;
		}

		if (!Class->IsChildOf(InRequiredBase))
		{
			OutError = FString::Printf(TEXT("`%s` 不是 %s 的派生类"), *InClassName, InRequiredBaseName);
			return nullptr;
		}

		return Class;
	}

	/**
	 * 两个属性集合是否等价。
	 *
	 * 「没有这个集合」与「空的集合」必须视为相同：前者是配置里没写 props，后者是导出时
	 * 没有可写的属性——它们表达同一件事，判为不同的话每次回环都会误报「已更新」。
	 */
	bool AreJsonObjectsEquivalent(const TSharedPtr<FJsonObject>& InLeft, const TSharedPtr<FJsonObject>& InRight)
	{
		const bool bLeftEmpty = !InLeft.IsValid() || InLeft->Values.Num() == 0;
		const bool bRightEmpty = !InRight.IsValid() || InRight->Values.Num() == 0;
		if (bLeftEmpty && bRightEmpty)
		{
			return true;
		}

		return ForgeJsonText::ToCanonicalString(InLeft) == ForgeJsonText::ToCanonicalString(InRight);
	}

	/**
	 * 归一化：删掉「等于默认值」的冗余字段，但关键参数要留着——先摘出来、Prune 完放回去。
	 * 与 texture 同款：显式写默认值与省略在比较时等价，关键参数则永远可见。
	 */
	void NormalizeProps(UClass* InClass, TSharedPtr<FJsonObject>& InOutProps, const TArray<FName>& InKeyPropertyNames)
	{
		TMap<FString, TSharedPtr<FJsonValue>> PreservedKeyValues;
		if (InOutProps.IsValid())
		{
			for (const FName& KeyName : InKeyPropertyNames)
			{
				const FString KeyString = KeyName.ToString();
				if (InOutProps->HasField(KeyString))
				{
					PreservedKeyValues.Add(KeyString, InOutProps->TryGetField(KeyString));
				}
			}
		}

		ForgePropertyCodec::PruneDefaults(InClass, InOutProps);

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : PreservedKeyValues)
		{
			if (!InOutProps.IsValid())
			{
				InOutProps = MakeShared<FJsonObject>();
			}
			InOutProps->SetField(Pair.Key, Pair.Value);
		}
	}

	/** 关键参数缺失只提醒不阻断（导出方向永远写全；这条只会在手写配置漏项时命中）。 */
	void WarnMissingKeyProperties(
		const TSharedPtr<FJsonObject>& InProps,
		const TArray<FName>& InKeyPropertyNames,
		const FString& InFieldPath,
		FForgeReport& OutReport)
	{
		TArray<FString> MissingKeys;
		for (const FName& KeyName : InKeyPropertyNames)
		{
			if (!InProps.IsValid() || !InProps->HasField(KeyName.ToString()))
			{
				MissingKeys.Add(KeyName.ToString());
			}
		}

		if (MissingKeys.Num() > 0)
		{
			OutReport.AddWarning(InFieldPath, FString::Printf(
				TEXT("关键参数没有显式定义：%s（没定义就用引擎默认/资产现值；建议补全）"),
				*FString::Join(MissingKeys, TEXT("、"))));
		}
	}

	/** 把 UFont 读成节点（导出方向；导入时比对当前状态也读它）。 */
	void ReadFontRootInto(const UFont* InFont, FForgeNode& OutNode, TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		OutNode.ClassName = FontClassShort;
		OutNode.Name = InFont->GetName();
		ForgePropertyCodec::ExportDeltaProperties(
			InFont, OutNode.Props, OutDiagnostics, TEXT("root.props"), &FontExcludedPropertyNames());

		ForgePropertyCodec::ExportForcedProperties(
			InFont, FontKeyPropertyNames(), OutNode.Props, OutDiagnostics, TEXT("root.props"));
	}

	/** 把 UFontFace 读成节点（source 写回扩展字段；调用方补 typeface 名）。 */
	void ReadFaceInto(
		const UFontFace* InFace,
		const FString& InNodePath,
		FForgeNode& OutNode,
		TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		OutNode.ClassName = FontFaceClassShort;
		OutNode.Name = InFace->GetName();

		const FString PropsPath = FString::Printf(TEXT("%s.props"), *InNodePath);
		ForgePropertyCodec::ExportDeltaProperties(
			InFace, OutNode.Props, OutDiagnostics, PropsPath, &FaceExcludedPropertyNames());

		ForgePropertyCodec::ExportForcedProperties(
			InFace, FaceKeyPropertyNames(), OutNode.Props, OutDiagnostics, PropsPath);

		if (!InFace->SourceFilename.IsEmpty())
		{
			if (!OutNode.Extra.IsValid())
			{
				OutNode.Extra = MakeShared<FJsonObject>();
			}
			OutNode.Extra->SetStringField(FaceSourceField, ForgePaths::MakeProjectRelative(InFace->SourceFilename));
		}
	}

	/** 当前默认 typeface 的面列表（含 entry 名；用于顺序比较与按资产名查找）。 */
	TArray<FCurrentFace> ReadCurrentFaces(const UFont* InFont)
	{
		TArray<FCurrentFace> Result;
		for (const FTypefaceEntry& Entry : InFont->GetInternalCompositeFont().DefaultTypeface.Fonts)
		{
			FCurrentFace& Item = Result.AddDefaulted_GetRef();
			Item.EntryName = Entry.Name;
			Item.Face = Cast<UFontFace>(Entry.Font.GetFontFaceAsset());
		}
		return Result;
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

	/** children 的一项 → 面规格；解析出的内容全部校验过。 */
	bool ParseFaceSpec(
		const FForgeNode& InNode,
		const FString& InNodePath,
		FFaceSpec& OutSpec,
		TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		bool bOk = true;

		FString ClassError;
		if (ResolveRequiredClass(InNode.ClassName, UFontFace::StaticClass(), TEXT("UFontFace"), ClassError) == nullptr)
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Error, FString::Printf(TEXT("%s.class"), *InNodePath), ClassError);
			bOk = false;
		}

		OutSpec.AssetName = InNode.Name;
		OutSpec.Props = InNode.Props;

		if (InNode.Extra.IsValid())
		{
			InNode.Extra->TryGetStringField(FaceTypefaceField, OutSpec.TypefaceName);
			InNode.Extra->TryGetStringField(FaceSourceField, OutSpec.SourceConfig);
		}

		if (OutSpec.TypefaceName.IsEmpty())
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Error,
				FString::Printf(TEXT("%s.typeface"), *InNodePath),
				TEXT("缺少必需字段 `typeface`：它是在 UFont 里选择这一面的名字（如 Regular / Bold / Italic）"));
			bOk = false;
		}

		if (OutSpec.SourceConfig.IsEmpty())
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Error,
				FString::Printf(TEXT("%s.source"), *InNodePath),
				TEXT("缺少必需字段 `source`：字体面的内容来自 ttf 文件（相对项目根的路径）"));
			bOk = false;
		}
		else
		{
			OutSpec.SourcePath = ForgePaths::CollapseToAbsolute(OutSpec.SourceConfig);
			if (!FPaths::FileExists(OutSpec.SourcePath))
			{
				OutDiagnostics.Emplace(
					EForgeSeverity::Error,
					FString::Printf(TEXT("%s.source"), *InNodePath),
					FString::Printf(TEXT("找不到字体文件：%s"), *OutSpec.SourcePath));
				bOk = false;
			}
		}

		if (InNode.Props.IsValid())
		{
			for (const auto& Pair : InNode.Props->Values)
			{
				const FString Key(ASSETFORGE_JSON_KEY(Pair.Key));
				if (IsExcludedProperty(FaceExcludedPropertyNames(), Key))
				{
					OutDiagnostics.Emplace(
						EForgeSeverity::Error,
						FString::Printf(TEXT("%s.props.%s"), *InNodePath, *Key),
						TEXT("源文件由 `source` 字段表达；不要在 props 里写它"));
					bOk = false;
				}
			}
		}

		return bOk;
	}

	/**
	 * 目标状态（配置）与当前状态（资产）的差异 → 计划。
	 *
	 * 粒度：根一条（UFont 属性或字体面列表变化）+ 每个面一条（新建/更新/移除引用）。
	 * 面按**资产名**匹配（name 是配置的匹配键，同 UMG 的控件名）。
	 */
	void ComputeFontPlan(
		const FForgeDocument& InDocument,
		const TArray<FFaceSpec>& InSpecs,
		const UFont* InCurrentFont,
		TArray<FForgePlanEntry>& OutPlan,
		TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		if (InCurrentFont == nullptr)
		{
			OutPlan.Add({ EForgeAction::Create, TEXT("root"), InDocument.Root.Name,
				FString::Printf(TEXT("新建字体 %s"), *InDocument.Asset) });

			for (const FFaceSpec& Spec : InSpecs)
			{
				OutPlan.Add({ EForgeAction::Create,
					FString::Printf(TEXT("root/%s"), *Spec.AssetName),
					Spec.AssetName,
					FString::Printf(TEXT("新建字体面（typeface %s）"), *Spec.TypefaceName) });
			}
			return;
		}

		const TArray<FCurrentFace> CurrentFaces = ReadCurrentFaces(InCurrentFont);

		TMap<FString, const FCurrentFace*> CurrentByAssetName;
		for (const FCurrentFace& Item : CurrentFaces)
		{
			if (Item.Face != nullptr)
			{
				CurrentByAssetName.Add(Item.Face->GetName(), &Item);
			}
		}

		// ---- 根：UFont 属性 + 字体面列表 ----
		{
			TArray<FString> Details;

			FForgeNode CurrentRoot;
			ReadFontRootInto(InCurrentFont, CurrentRoot, OutDiagnostics);
			if (!AreJsonObjectsEquivalent(InDocument.Root.Props, CurrentRoot.Props))
			{
				Details.Add(ForgeDiff::DescribeObjectDiff(InDocument.Root.Props, CurrentRoot.Props, TEXT("props")));
			}

			TArray<FString> CurrentOrder;
			for (const FCurrentFace& Item : CurrentFaces)
			{
				CurrentOrder.Add(Item.Face != nullptr ? Item.Face->GetName() : Item.EntryName.ToString());
			}

			TArray<FString> TargetOrder;
			TargetOrder.Reserve(InSpecs.Num());
			for (const FFaceSpec& Spec : InSpecs)
			{
				TargetOrder.Add(Spec.AssetName);
			}

			if (CurrentOrder != TargetOrder)
			{
				Details.Add(FString::Printf(
					TEXT("字体面列表: [%s] → [%s]"),
					*FString::Join(CurrentOrder, TEXT(", ")),
					*FString::Join(TargetOrder, TEXT(", "))));
			}

			if (Details.Num() > 0)
			{
				OutPlan.Add({ EForgeAction::Update, TEXT("root"), InDocument.Root.Name,
					FString::Join(Details, TEXT("; ")) });
			}
		}

		// ---- 每个目标面 ----
		TSet<FString> TargetAssetNames;
		TargetAssetNames.Reserve(InSpecs.Num());

		for (const FFaceSpec& Spec : InSpecs)
		{
			TargetAssetNames.Add(Spec.AssetName);
			const FString NodePath = FString::Printf(TEXT("root/%s"), *Spec.AssetName);

			const FCurrentFace* const* Found = CurrentByAssetName.Find(Spec.AssetName);
			if (Found == nullptr)
			{
				OutPlan.Add({ EForgeAction::Create, NodePath, Spec.AssetName,
					FString::Printf(TEXT("新建字体面（typeface %s）"), *Spec.TypefaceName) });
				continue;
			}

			TArray<FString> Details;

			FForgeNode CurrentNode;
			ReadFaceInto((*Found)->Face, NodePath, CurrentNode, OutDiagnostics);
			if (!AreJsonObjectsEquivalent(Spec.Props, CurrentNode.Props))
			{
				Details.Add(ForgeDiff::DescribeObjectDiff(Spec.Props, CurrentNode.Props, TEXT("props")));
			}

			if ((*Found)->EntryName.ToString() != Spec.TypefaceName)
			{
				Details.Add(FString::Printf(
					TEXT("typeface: %s → %s"), *(*Found)->EntryName.ToString(), *Spec.TypefaceName));
			}

			const FString CurrentSource = (*Found)->Face->SourceFilename.IsEmpty()
				? FString()
				: ForgePaths::CollapseToAbsolute((*Found)->Face->SourceFilename);
			if (CurrentSource != Spec.SourcePath)
			{
				Details.Add(FString::Printf(
					TEXT("source: %s → %s"),
					CurrentSource.IsEmpty() ? TEXT("（无）") : *ForgePaths::MakeProjectRelative(CurrentSource),
					*Spec.SourceConfig));
			}

			if (Details.Num() > 0)
			{
				OutPlan.Add({ EForgeAction::Update, NodePath, Spec.AssetName, FString::Join(Details, TEXT("; ")) });
			}
		}

		// ---- 当前引用了、配置里没有的面：移除引用（资产文件保留，见计划里的说明）----
		for (const FCurrentFace& Item : CurrentFaces)
		{
			const FString FaceName = Item.Face != nullptr ? Item.Face->GetName() : Item.EntryName.ToString();
			if (TargetAssetNames.Contains(FaceName))
			{
				continue;
			}

			OutPlan.Add({ EForgeAction::Delete,
				FString::Printf(TEXT("root/%s"), *FaceName),
				FaceName,
				TEXT("配置中已移除：将从字体面列表移除；资产文件保留，需删除请在内容浏览器里手工处理") });
		}
	}

	/** 建/更新一个字体面并保存。bOutReloaded 报告字体数据是否被重灌（决定要不要刷 Slate 缓存）。 */
	bool ExecuteFace(
		const FString& InFontAssetPath,
		const FFaceSpec& InSpec,
		bool& bOutReloaded,
		FForgeReport& OutReport)
	{
		bOutReloaded = false;

		const FString FacePath = FaceAssetPathOf(InFontAssetPath, InSpec.AssetName);
		const FString NodePath = FString::Printf(TEXT("root/%s"), *InSpec.AssetName);

		UFontFace* Face = nullptr;
		if (FPackageName::DoesPackageExist(FacePath))
		{
			Face = LoadObject<UFontFace>(nullptr, *FacePath);
			if (Face == nullptr)
			{
				OutReport.AddError(NodePath, FString::Printf(
					TEXT("`%s` 存在但不是 FontFace；该路径已被别的资产占用"), *FacePath));
				return false;
			}
		}

		// 数据要不要重灌：新建、源路径变化、或源文件比上次保存的资产新。
		// 字体的源数据没有导入记录可用（UFontFace 上没有 UAssetImportData），时间戳基准
		// 就取资产文件自己的修改时间——改了 ttf 再导入即可生效，无需先动配置。
		bool bReload = false;
		if (Face == nullptr)
		{
			bReload = true;
		}
		else
		{
			const FString CurrentSource = Face->SourceFilename.IsEmpty()
				? FString()
				: ForgePaths::CollapseToAbsolute(Face->SourceFilename);
			if (CurrentSource != InSpec.SourcePath)
			{
				bReload = true;
			}
			else
			{
				const FString PackageFileName = FPackageName::LongPackageNameToFilename(
					FacePath, FPackageName::GetAssetPackageExtension());
				const FDateTime SavedAt = IFileManager::Get().GetTimeStamp(*PackageFileName);
				const FDateTime SourceStamp = IFileManager::Get().GetTimeStamp(*InSpec.SourcePath);
				if (SavedAt == FDateTime::MinValue() || SourceStamp > SavedAt + FTimespan::FromSeconds(1.0))
				{
					bReload = true;
				}
			}
		}

		if (Face == nullptr)
		{
			UPackage* Package = CreatePackage(*FacePath);
			if (Package == nullptr)
			{
				OutReport.AddError(NodePath, FString::Printf(TEXT("无法创建包：%s"), *FacePath));
				return false;
			}

			Face = NewObject<UFontFace>(Package, FName(*InSpec.AssetName), RF_Public | RF_Standalone);
			if (Face == nullptr)
			{
				OutReport.AddError(NodePath, FString::Printf(TEXT("创建字体面失败：%s"), *FacePath));
				return false;
			}

			FAssetRegistryModule::AssetCreated(Face);
		}

		if (bReload)
		{
			TArray<uint8> FileData;
			if (!FFileHelper::LoadFileToArray(FileData, *InSpec.SourcePath))
			{
				OutReport.AddError(FString::Printf(TEXT("%s.source"), *NodePath), FString::Printf(
					TEXT("读不到字体文件：%s"), *InSpec.SourcePath));
				return false;
			}

			// 数据本体就是 ttf 字节流——引擎自己的导入走的也是这条路（FontFaceData->SetData）。
			Face->FontFaceData->SetData(MoveTemp(FileData));
			bOutReloaded = true;
		}

		// 先回默认再应用配置：配置没写的属性不该在资产里残留旧值。
		ForgePropertyCodec::ResetToDefaults(Face);
		ForgePropertyCodec::ImportProperties(
			Face, InSpec.Props, OutReport.Diagnostics, FString::Printf(TEXT("%s.props"), *NodePath));

		// SourceFilename 不在 props 通道里（见 FaceExcludedPropertyNames）；属性重置会清掉它，单独写回。
		Face->SourceFilename = InSpec.SourcePath;

		// 子面缓存（编辑器专用；commandlet 下 Slate 未初始化，函数内部会跳过）。
		Face->CacheSubFaces();

		if (OutReport.HasErrors())
		{
			return false;
		}

		return SaveAssetPackage(Face, OutReport);
	}

	/** 建/更新 UFont 本身：属性 + 按配置顺序整体重建 typeface 列表。 */
	bool ExecuteFont(const FForgeDocument& InDocument, const TArray<FFaceSpec>& InSpecs, FForgeReport& OutReport)
	{
		UFont* Font = LoadObject<UFont>(nullptr, *InDocument.Asset);
		if (Font == nullptr)
		{
			const FString AssetName = AssetNameOf(InDocument.Asset);
			if (AssetName.IsEmpty())
			{
				OutReport.AddError(TEXT("asset"), FString::Printf(
					TEXT("资产路径 `%s` 不合法，应形如 /Game/.../F_Xxx"), *InDocument.Asset));
				return false;
			}

			UPackage* Package = CreatePackage(*InDocument.Asset);
			if (Package == nullptr)
			{
				OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("无法创建包：%s"), *InDocument.Asset));
				return false;
			}

			Font = NewObject<UFont>(Package, FName(*AssetName), RF_Public | RF_Standalone);
			if (Font == nullptr)
			{
				OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("创建字体失败：%s"), *InDocument.Asset));
				return false;
			}

			FAssetRegistryModule::AssetCreated(Font);
		}

		// 先回默认再应用配置：CompositeFont 一并回到空，随后按配置整体重建。
		ForgePropertyCodec::ResetToDefaults(Font);
		ForgePropertyCodec::ImportProperties(Font, InDocument.Root.Props, OutReport.Diagnostics, TEXT("root.props"));

		FCompositeFont& Composite = Font->GetMutableInternalCompositeFont();
		Composite.DefaultTypeface.Fonts.Empty();
		// 子字体（按字符范围回退）本 kind 不表达：清掉，保证「配置是唯一真源」。
		Composite.SubTypefaces.Empty();

		for (const FFaceSpec& Spec : InSpecs)
		{
			UFontFace* Face = LoadObject<UFontFace>(nullptr, *FaceAssetPathOf(InDocument.Asset, Spec.AssetName));
			if (Face == nullptr)
			{
				OutReport.AddError(TEXT("root.children"), FString::Printf(
					TEXT("字体面 `%s` 刚保存却在磁盘上找不到——保存失败或名称不一致"), *Spec.AssetName));
				return false;
			}

			FTypefaceEntry& Entry = Composite.DefaultTypeface.Fonts.AddDefaulted_GetRef();
			Entry.Name = FName(*Spec.TypefaceName);
			Entry.Font = FFontData(Face);
		}

		if (OutReport.HasErrors())
		{
			return false;
		}

		return SaveAssetPackage(Font, OutReport);
	}
}

bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport)
{
	OutReport.AssetPath = InAssetPath;

	UFont* Font = LoadObject<UFont>(nullptr, *InAssetPath);
	if (Font == nullptr)
	{
		OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("找不到字体资产：%s"), *InAssetPath));
		return false;
	}

	if (Font->FontCacheType != EFontCacheType::Runtime)
	{
		OutReport.AddError(TEXT("root.props.FontCacheType"),
			TEXT("本 kind 只支持 Runtime 缓存的字体；离线烘焙字体（字形贴图）不归配置管"));
		return false;
	}

	OutDocument.Kind = Kind;
	OutDocument.SchemaVersion = ForgeDoc::CurrentSchemaVersion;
	OutDocument.Asset = InAssetPath;

	ReadFontRootInto(Font, OutDocument.Root, OutReport.Diagnostics);

	const FCompositeFont& Composite = Font->GetInternalCompositeFont();
	if (Composite.SubTypefaces.Num() > 0)
	{
		OutReport.AddWarning(TEXT("root"),
			TEXT("资产的复合字体含子字体（SubTypefaces），本 kind 不表达它们——导入会清空"));
	}

	int32 Index = 0;
	for (const FTypefaceEntry& Entry : Composite.DefaultTypeface.Fonts)
	{
		const FString ChildPath = FString::Printf(TEXT("root.children[%d]"), Index);
		++Index;

		const UFontFace* Face = Cast<UFontFace>(Entry.Font.GetFontFaceAsset());
		if (Face == nullptr)
		{
			OutReport.AddWarning(ChildPath, FString::Printf(
				TEXT("entry `%s` 不是 FontFace 资产（旧式裸文件名引用），无法导出"), *Entry.Name.ToString()));
			continue;
		}

		FForgeNode& Child = OutDocument.Root.Children.AddDefaulted_GetRef();
		ReadFaceInto(Face, ChildPath, Child, OutReport.Diagnostics);

		if (!Child.Extra.IsValid())
		{
			Child.Extra = MakeShared<FJsonObject>();
		}
		Child.Extra->SetStringField(FaceTypefaceField, Entry.Name.ToString());
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

	FString ClassError;
	UClass* FontClass = ResolveRequiredClass(
		InDocument.Root.ClassName, UFont::StaticClass(), TEXT("UFont"), ClassError);
	if (FontClass == nullptr)
	{
		OutReport.AddError(TEXT("root.class"), ClassError);
		return false;
	}

	if (InDocument.Root.Children.Num() == 0)
	{
		OutReport.AddError(TEXT("root.children"),
			TEXT("字体至少需要一个字体面（children）；空字体没有可表达的内容"));
	}

	// 结构属性与 FName 属性不能走配置（见 FontExcludedPropertyNames）。
	if (InDocument.Root.Props.IsValid())
	{
		for (const auto& Pair : InDocument.Root.Props->Values)
		{
			const FString Key(ASSETFORGE_JSON_KEY(Pair.Key));
			if (!IsExcludedProperty(FontExcludedPropertyNames(), Key))
			{
				continue;
			}

			OutReport.AddError(FString::Printf(TEXT("root.props.%s"), *Key),
				Key == LegacyFontNameProperty
					? TEXT("FName 属性不被 JSON 属性通道支持；字体面的选择走 UMG 里的 Typeface 名")
					: TEXT("字体结构由 children 表达；不要在 props 里直接写复合字体"));
		}
	}

	// FontCacheType 必须显式写 Runtime：CDO 默认是 Offline，省略会被重置成离线烘焙模式。
	{
		FString CacheType;
		if (InDocument.Root.Props.IsValid())
		{
			InDocument.Root.Props->TryGetStringField(FontCacheTypeProperty, CacheType);
		}

		if (!CacheType.Equals(TEXT("Runtime"), ESearchCase::IgnoreCase))
		{
			OutReport.AddError(TEXT("root.props.FontCacheType"),
				TEXT("必须显式写 \"Runtime\"：本 kind 只生成运行时缓存的复合字体（离线烘焙字体不归配置管）"));
		}
	}

	TArray<FFaceSpec> Specs;
	Specs.Reserve(InDocument.Root.Children.Num());
	{
		int32 Index = 0;
		for (const FForgeNode& Child : InDocument.Root.Children)
		{
			const FString NodePath = FString::Printf(TEXT("root.children[%d]"), Index);
			++Index;

			FFaceSpec Spec;
			if (ParseFaceSpec(Child, NodePath, Spec, OutReport.Diagnostics))
			{
				Specs.Add(MoveTemp(Spec));
			}
		}
	}

	// 重名：name 是导入时的匹配键、typeface 是 Slate 的选择键，两者都必须全局唯一。
	{
		TSet<FString> SeenAssetNames;
		TSet<FString> SeenTypefaces;
		for (const FFaceSpec& Spec : Specs)
		{
			bool bDuplicate = false;
			SeenAssetNames.Add(Spec.AssetName, &bDuplicate);
			if (bDuplicate)
			{
				OutReport.AddError(TEXT("root.children"), FString::Printf(
					TEXT("字体面名 `%s` 重复：name 是导入时的匹配键"), *Spec.AssetName));
			}

			bDuplicate = false;
			SeenTypefaces.Add(Spec.TypefaceName, &bDuplicate);
			if (bDuplicate)
			{
				OutReport.AddError(TEXT("root.children"), FString::Printf(
					TEXT("typeface 名 `%s` 重复：UFont 里两个同名 entry 无法区分"), *Spec.TypefaceName));
			}
		}
	}

	// 先校验再归一化：校验看作者写了什么，归一化改的是拿去比较的形态。
	ForgePropertyCodec::ValidateProperties(
		FontClass, InDocument.Root.Props, OutReport.Diagnostics, TEXT("root.props"));

	for (int32 Index = 0; Index < Specs.Num(); ++Index)
	{
		ForgePropertyCodec::ValidateProperties(
			UFontFace::StaticClass(),
			Specs[Index].Props,
			OutReport.Diagnostics,
			FString::Printf(TEXT("root.children[%d].props"), Index));
	}

	// FontCacheType 是硬性要求（缺了上面已报错），根部不需要再叠一条"关键参数缺失"的警告。
	NormalizeProps(FontClass, InDocument.Root.Props, FontKeyPropertyNames());

	for (int32 Index = 0; Index < Specs.Num(); ++Index)
	{
		NormalizeProps(UFontFace::StaticClass(), Specs[Index].Props, FaceKeyPropertyNames());
		WarnMissingKeyProperties(
			Specs[Index].Props,
			FaceKeyPropertyNames(),
			FString::Printf(TEXT("root.children[%d].props"), Index),
			OutReport);
	}

	// ---- 当前状态 ----

	UFont* CurrentFont = nullptr;
	if (FPackageName::DoesPackageExist(InDocument.Asset))
	{
		CurrentFont = LoadObject<UFont>(nullptr, *InDocument.Asset);
		if (CurrentFont == nullptr)
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(
				TEXT("`%s` 存在但不是 UFont；font 配置的目标只能是字体资产"), *InDocument.Asset));
			return false;
		}

		// 离线烘焙字体的内容（字形贴图 + 字符表）不是配置能表达的东西，按配置重建会把它弄坏。
		if (CurrentFont->FontCacheType != EFontCacheType::Runtime)
		{
			OutReport.AddError(TEXT("root.props.FontCacheType"),
				TEXT("目标字体不是 Runtime 缓存（离线烘焙字体不归配置管）；先在编辑器里改为 Runtime，或换一个资产路径"));
			return false;
		}
	}

	// ---- 算计划 ----

	ComputeFontPlan(InDocument, Specs, CurrentFont, OutReport.Actions, OutReport.Diagnostics);
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

	// 先面后字（字体引用字体面；面没落盘就存字体，重启后引用会丢）。
	bool bAnyFaceReloaded = false;
	for (const FFaceSpec& Spec : Specs)
	{
		bool bReloaded = false;
		if (!ExecuteFace(InDocument.Asset, Spec, bReloaded, OutReport))
		{
			return false;
		}
		bAnyFaceReloaded |= bReloaded;
	}

	if (!ExecuteFont(InDocument, Specs, OutReport))
	{
		return false;
	}

	// 字体数据换过 → Slate 的字形缓存要作废，否则编辑器里还是旧字形。
	if (bAnyFaceReloaded && FSlateApplication::IsInitialized())
	{
		if (FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer())
		{
			Renderer->FlushFontCache(TEXT("FontForge::ImportDocument"));
		}
	}

	OutReport.bSucceeded = true;
	return true;
}

} // namespace FontForge
