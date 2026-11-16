// Copyright Epic Games, Inc. All Rights Reserved.

#include "Kinds/Map/MapForge.h"
#include "AssetForgeCompat.h"

#include "Core/ForgeDiff.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgePropertyCodec.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#if ASSETFORGE_HAS_NEW_MAP_WORLD_PARTITION_SETTINGS
#include "WorldPartition/WorldPartitionSettings.h"
#endif

namespace MapForge
{

namespace
{
	/** 配置里 root.name 的固定写法：世界设置这个对象在地图里没有别的名字可言。 */
	const TCHAR* const WorldSettingsNodeName = TEXT("WorldSettings");

	/**
	 * 找地图：优先内存里已加载的那份（用户正开着这张图时，它才是"现在的样子"），否则从磁盘加载。
	 *
	 * 刻意不用 `GEditor->LoadMap`——那会切换编辑器当前编辑的地图，是个纯副作用；
	 * 这里只要 World 对象本身。
	 */
	UWorld* FindWorld(const FString& InAssetPath)
	{
		if (InAssetPath.IsEmpty())
		{
			return nullptr;
		}

		if (UPackage* ExistingPackage = FindPackage(nullptr, *InAssetPath))
		{
			if (UWorld* LoadedWorld = UWorld::FindWorldInPackage(ExistingPackage))
			{
				return LoadedWorld;
			}
		}

		UPackage* Package = LoadPackage(nullptr, *InAssetPath, LOAD_None);
		return Package != nullptr ? UWorld::FindWorldInPackage(Package) : nullptr;
	}

	/**
	 * 世界设置对象；没有就返回 nullptr（由调用方报错）。
	 *
	 * 两级兜底：`UWorld::GetWorldSettings` 走当前关卡，`ULevel::GetWorldSettings` 走持久关卡的缓存；
	 * 对已保存的地图两者都应成立，但地图是可以被手改坏的，这里不拿 check 赌。
	 */
	AWorldSettings* FindWorldSettings(UWorld* InWorld)
	{
		if (InWorld == nullptr)
		{
			return nullptr;
		}

		if (AWorldSettings* Settings = InWorld->GetWorldSettings(/*bCheckStreamingPersistent=*/false, /*bChecked=*/false))
		{
			return Settings;
		}

		return InWorld->PersistentLevel != nullptr
			? InWorld->PersistentLevel->GetWorldSettings(/*bChecked=*/false)
			: nullptr;
	}

	/** 把一份世界设置读成节点（导出方向；导入的比对也读它）。 */
	void ReadSettingsInto(const AWorldSettings* InSettings, FForgeNode& OutNode, TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		OutNode.Name = WorldSettingsNodeName;
		OutNode.ClassName = InSettings->GetClass()->GetPathName();
		ForgePropertyCodec::ExportDeltaProperties(InSettings, OutNode.Props, OutDiagnostics, TEXT("root.props"));
	}

	/** 解析配置声明的世界设置类；只接受 `AWorldSettings` 家族。 */
	UClass* ResolveSettingsClass(const FString& InClassName, FString& OutError)
	{
		UClass* Class = LoadClass<UObject>(nullptr, *InClassName);
		if (Class == nullptr)
		{
			OutError = FString::Printf(TEXT("找不到世界设置类 `%s`"), *InClassName);
			return nullptr;
		}

		if (!Class->IsChildOf(AWorldSettings::StaticClass()))
		{
			OutError = FString::Printf(
				TEXT("`%s` 不是 AWorldSettings 的派生类；地图的世界设置只能是 WorldSettings 家族"),
				*InClassName);
			return nullptr;
		}

		return Class;
	}

	/** 新建一张只含世界设置的空地图；失败返回 nullptr 并把原因写进报告。 */
	UWorld* CreateEmptyWorld(const FString& InAssetPath, FForgeReport& OutReport)
	{
		FString PackagePath;
		FString AssetName;
		InAssetPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (AssetName.IsEmpty())
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(
				TEXT("资产路径 `%s` 不合法，应形如 /Game/Maps/Xxx"), *InAssetPath));
			return nullptr;
		}

		UPackage* Package = CreatePackage(*InAssetPath);
		if (Package == nullptr)
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("无法创建包：%s"), *InAssetPath));
			return nullptr;
		}

		// 与编辑器的「新建空白关卡」同一组初始化参数（取自 UWorldFactory 的默认值），
		// 世界类型是 Inactive：这是一份磁盘资产，不该被引擎当成"已加载的世界"。
#if ASSETFORGE_HAS_NEW_MAP_WORLD_PARTITION_SETTINGS
		const UWorldPartitionSettings* WorldPartitionSettings = UWorldPartitionSettings::Get();
		const bool bCreateWorldPartition = WorldPartitionSettings->GetNewMapsEnableWorldPartition();
		const bool bWorldPartitionStreaming = WorldPartitionSettings->GetNewMapsEnableWorldPartitionStreaming();
#else
		// 5.4 没有「新建地图默认开世界分区」的项目设置：和编辑器「新建空白关卡」一样不开
		const bool bCreateWorldPartition = false;
		const bool bWorldPartitionStreaming = false;
#endif
		const UWorld::InitializationValues InitValues = UWorld::InitializationValues()
			.ShouldSimulatePhysics(false)
			.EnableTraceCollision(true)
			.CreateNavigation(false)
			.CreateAISystem(false)
			.CreateWorldPartition(bCreateWorldPartition)
			.EnableWorldPartitionStreaming(bWorldPartitionStreaming);

		UWorld* NewWorld = UWorld::CreateWorld(
			EWorldType::Inactive,
			/*bInformEngineOfWorld=*/false,
			FName(*AssetName),
			Package,
			/*bAddToRoot=*/false,
			ERHIFeatureLevel::Num,
			&InitValues);

		if (NewWorld == nullptr)
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("创建地图失败：%s"), *InAssetPath));
			return nullptr;
		}

		NewWorld->SetFlags(RF_Public | RF_Standalone);
		FAssetRegistryModule::AssetCreated(NewWorld);

		return NewWorld;
	}

	bool SaveMapPackage(UWorld* InWorld, FForgeReport& OutReport)
	{
		UPackage* Package = InWorld->GetOutermost();
		Package->MarkPackageDirty();

		// 与 .uasset 的差别就在这个扩展名上：地图必须落成 .umap，否则下次打开就是"资产不存在"。
		const FString PackageFileName = FPaths::ConvertRelativePathToFull(
			FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetMapPackageExtension()));

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;   // 批量导入时不该弹窗（5.8 里旧名 SAVE_NoErrorUI）
		if (!UPackage::SavePackage(Package, InWorld, *PackageFileName, SaveArgs))
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("保存地图失败：%s"), *PackageFileName));
			return false;
		}

		return true;
	}
}

bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport)
{
	OutReport.AssetPath = InAssetPath;

	UWorld* World = FindWorld(InAssetPath);
	if (World == nullptr)
	{
		OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("找不到地图资产：%s"), *InAssetPath));
		return false;
	}

	AWorldSettings* Settings = FindWorldSettings(World);
	if (Settings == nullptr)
	{
		OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("地图里没有世界设置：%s"), *InAssetPath));
		return false;
	}

	OutDocument.Kind = Kind;
	OutDocument.SchemaVersion = ForgeDoc::CurrentSchemaVersion;
	OutDocument.Asset = InAssetPath;

	ReadSettingsInto(Settings, OutDocument.Root, OutReport.Diagnostics);

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
			TEXT("地图配置不描述关卡内容：只有 root.props（世界设置）。关卡里摆什么请在编辑器里做"));
	}

	FString ClassError;
	UClass* SettingsClass = ResolveSettingsClass(InDocument.Root.ClassName, ClassError);
	if (SettingsClass == nullptr)
	{
		OutReport.AddError(TEXT("root.class"), ClassError);
		return false;
	}

	// 先校验再归一化：校验要看的是作者写了什么，归一化改的是拿去比较的形态。
	ForgePropertyCodec::ValidateProperties(
		SettingsClass,
		InDocument.Root.Props,
		OutReport.Diagnostics,
		TEXT("root.props"));
	ForgePropertyCodec::PruneDefaults(SettingsClass, InDocument.Root.Props);

	// ---- 找现状、算计划 ----

	UWorld* World = nullptr;
	if (FPackageName::DoesPackageExist(InDocument.Asset))
	{
		World = FindWorld(InDocument.Asset);
		if (World == nullptr)
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(
				TEXT("`%s` 存在但不是地图；map 配置的目标只能是 .umap"), *InDocument.Asset));
			return false;
		}
	}

	AWorldSettings* ExistingSettings = FindWorldSettings(World);
	if (World != nullptr && ExistingSettings == nullptr)
	{
		OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("地图里没有世界设置：%s"), *InDocument.Asset));
		return false;
	}

	// 世界设置的类由地图决定（`UWorld::WorldSettingsClass`）；配置写得对不上时明确报错，
	// 而不是按配置的类去改——那会让反射写入落到错误的对象上。
	if (ExistingSettings != nullptr && ExistingSettings->GetClass() != SettingsClass)
	{
		OutReport.AddError(
			TEXT("root.class"),
			FString::Printf(
				TEXT("地图 `%s` 的世界设置是 `%s`，与配置的 `%s` 不一致；换世界设置类要在编辑器里做"),
				*InDocument.Asset,
				*ExistingSettings->GetClass()->GetPathName(),
				*InDocument.Root.ClassName));
		return false;
	}

	// 计划：新地图是全量创建，已有地图按世界设置的属性差异算更新。
	if (World == nullptr)
	{
		OutReport.Actions.Add({ EForgeAction::Create, TEXT("root"), WorldSettingsNodeName, FString() });
	}
	else
	{
		FForgeNode CurrentNode;
		ReadSettingsInto(ExistingSettings, CurrentNode, OutReport.Diagnostics);
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

	// ---- 执行 ----

	AWorldSettings* Settings = ExistingSettings;
	if (Settings == nullptr)
	{
		World = CreateEmptyWorld(InDocument.Asset, OutReport);
		if (World == nullptr)
		{
			return false;
		}

		Settings = FindWorldSettings(World);
		if (Settings == nullptr)
		{
			OutReport.AddError(TEXT("asset"), TEXT("新建的地图里没有世界设置——这是不该发生的情况，请上报"));
			return false;
		}

		if (Settings->GetClass() != SettingsClass)
		{
			OutReport.AddError(
				TEXT("root.class"),
				FString::Printf(
					TEXT("新建地图的世界设置是 `%s`，与配置的 `%s` 不一致"),
					*Settings->GetClass()->GetPathName(),
					*InDocument.Root.ClassName));
			return false;
		}
	}

	// 没有动作就不要写盘：无意义的保存会改时间戳、触发下游重导，也让"改动"这件事失真。
	if (OutReport.Actions.Num() == 0)
	{
		OutReport.bSucceeded = true;
		return true;
	}

	// 先回默认再应用配置：配置没写的属性不该在资产里残留旧值。
	ForgePropertyCodec::ResetToDefaults(Settings);
	ForgePropertyCodec::ImportProperties(
		Settings,
		InDocument.Root.Props,
		OutReport.Diagnostics,
		TEXT("root.props"));

	if (OutReport.HasErrors())
	{
		return false;
	}

	if (!SaveMapPackage(World, OutReport))
	{
		return false;
	}

	OutReport.bSucceeded = true;
	return true;
}

} // namespace MapForge
