// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"
#include "State/ForgeState.h"

/**
 * 主列表里的一行：一份配置，或一个还没有配置的资产（未纳管）。
 *
 * 面板的 `forge.entries`、命令行 `-status` 用的是同一份数据——两处看到的状态必然一致。
 */
struct ASSETFORGE_API FForgeEntry
{
	/** 稳定键：配置 = 相对配置根的路径；未纳管资产 = 资产包名。 */
	FString Key;

	/** 配置文件绝对路径；未纳管资产为空。 */
	FString ConfigPath;

	/** 相对配置根的路径（`umg/UI/WBP_HUD.json`）；未纳管资产为空。 */
	FString RelativePath;

	/** 目标资产包名（`/Game/UI/WBP_HUD`）。 */
	FString AssetPath;

	/** 本插件的 kind；外部配置时为空（见 ForeignKind）。 */
	FString Kind;

	/** 外部配置的 kind 原文。 */
	FString ForeignKind;

	FString ParentClass;
	int32 Revision = 0;

	EForgeSyncState State = EForgeSyncState::Unknown;

	/** 解析失败原因等一句话说明。 */
	FString Message;

	/** 这份配置 include 的片段文件（绝对路径）；片段改了，这一项也算改了。 */
	TArray<FString> Fragments;

	bool IsConfig() const { return !ConfigPath.IsEmpty(); }
	bool HasAsset() const { return !AssetPath.IsEmpty(); }

	/** 列表里显示的名字：配置 = 文件名（不带扩展名）；资产 = 资产短名。 */
	FString GetDisplayName() const;

	/** 「能导入」：是本插件的、解析成功的配置。 */
	bool CanImport() const;

	/** 「能导出到配置」：有资产、资产类型受支持。 */
	bool CanExport() const;

	TSharedPtr<FJsonObject> ToJson() const;
};

/** 批量操作的结果。 */
struct ASSETFORGE_API FForgeBatchResult
{
	int32 Succeeded = 0;
	int32 Failed = 0;
	int32 Skipped = 0;
	bool bCancelled = false;

	/** 每一项的报告（成功与失败都在，顺序为处理完成的顺序）。 */
	TArray<FForgeReport> Reports;

	bool HasFailures() const { return Failed > 0; }
	FText GetSummary(const FText& InVerb) const;
	TSharedPtr<FJsonObject> ToJson() const;
};

/**
 * 所有入口共用的高层操作。
 *
 * 「一次用户操作」的完整语义只在这里写一遍：导入成功后记清单、导出时选落点并保留 revision、
 * 批量导入按依赖多轮推进。面板操作（forge.*）、命令行、内容浏览器右键菜单都调这里，不再各写各的。
 *
 * 全部在游戏线程调用（会加载 / 保存 UObject）。
 */
namespace ForgeService
{
	// ---- 扫描 ----

	/**
	 * 递归扫描配置根下的全部配置（跳过 `_`、`.` 开头的生成物和被 include 的片段文件），并给出快速同步状态。
	 * 一个片段被几份配置引用时，在这几项的 Message 里提示。
	 */
	ASSETFORGE_API TArray<FForgeEntry> ScanConfigs();

	/** 描述一份配置（解析 + 快速状态）。 */
	ASSETFORGE_API FForgeEntry DescribeConfig(const FString& InConfigPath);

	/**
	 * 扫描未纳管资产：设置里的扫描根下、类型受支持、且不在 `InManagedAssetPaths` 里的资产。
	 * 只读资产注册表，**不加载资产**。
	 */
	ASSETFORGE_API TArray<FForgeEntry> ScanUnmanagedAssets(const TSet<FString>& InManagedAssetPaths);

	/** 便捷：配置 + 未纳管资产（后者按需）。 */
	ASSETFORGE_API TArray<FForgeEntry> ScanAll(bool bInIncludeUnmanaged);

	// ---- 单项 ----

	/** 预览计划（dry-run）。会顺手把深度判定结果写进 `OutState`（可空）。 */
	ASSETFORGE_API FForgeReport Plan(const FString& InConfigPath, EForgeSyncState* OutState = nullptr);

	/** 预览一份还没存盘的配置（面板编辑器里的内容）：只演练，不改资产也不记清单。 */
	ASSETFORGE_API FForgeReport PlanText(const FString& InConfigPath, const FString& InConfigText);

	/** 导入一份配置；真跑成功后记清单。 */
	ASSETFORGE_API FForgeReport ImportConfig(const FString& InConfigPath, bool bInDryRun = false);

	/** 导入某个资产对应的配置（经清单反查 / 默认路径找到配置）。 */
	ASSETFORGE_API FForgeReport ImportAsset(const FString& InAssetPath, bool bInDryRun = false);

	/**
	 * 资产 → 配置（纳管 / 重新导出 / 导出为配置都是它）。
	 * 落点：清单里记得的那份 → 默认路径；写回既有配置时保留其 revision。导出后记清单（两边此刻一致）。
	 */
	ASSETFORGE_API FForgeReport ExportAsset(const FString& InAssetPath, FString* OutConfigPath = nullptr);

	/** 导出时的配置落点（不写文件）。 */
	ASSETFORGE_API FString ChooseExportConfigPath(const FString& InAssetPath, const FString& InKind);

	// ---- 批量 ----

	/**
	 * 批量导入（同步跑完）。引用另一份配置的 UMG 要等被引用者成为资产后才能解析，所以按「能过的先过」
	 * 多轮推进，直到某一轮毫无进展。`bInShowProgress` 时弹可取消的进度框。
	 * 不想卡住编辑器的（面板作业）用 `FForgeBatchRunner` 每帧走一步。
	 */
	ASSETFORGE_API FForgeBatchResult ImportConfigs(const TArray<FString>& InConfigPaths, bool bInDryRun, bool bInShowProgress);

	/** 批量导出（不支持的资产计入 Skipped，不算失败）。 */
	ASSETFORGE_API FForgeBatchResult ExportAssets(const TArray<FString>& InAssetPaths, bool bInShowProgress);

	/** 资产类型受支持、能导出成配置（只读资产注册表，不加载资产）。 */
	ASSETFORGE_API bool IsExportable(const FString& InAssetPath);

	/** 资产的 kind（只读资产注册表）；不支持的类型为空。 */
	ASSETFORGE_API FString KindOfAsset(const FString& InAssetPath);

	/**
	 * 批量深度检查：对快速判定拿不准的条目跑 dry-run，就地更新 `State`。
	 * 返回被检查的条目数；用户取消时提前返回。
	 */
	ASSETFORGE_API int32 DeepCheckEntries(TArray<FForgeEntry>& InOutEntries, bool bInShowProgress);

	/** 当前需要导入的配置（扫描 + `NeedsImport`）。 */
	ASSETFORGE_API TArray<FString> CollectPendingConfigs();
}
