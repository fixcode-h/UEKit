// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 同步状态：配置与资产之间「现在是什么关系」。
 *
 * 分两级：
 * - **快速**（`QuickState`）：只看清单里的内容哈希与资产文件修改时间，不加载资产——扫描整棵配置树用它。
 * - **深度**（`DeepCheck`）：真跑一次 dry-run，回答"导入会不会改变资产"。只对快速判定拿不准的
 *   （配置改过 / 资产改过）按需执行；结论是「已同步」时顺手刷新清单，下次快速判定就直接命中。
 */
enum class EForgeSyncState : uint8
{
	/** 还没判定。 */
	Unknown,
	/** 配置与资产一致。 */
	Synced,
	/** 有配置、从未导入过（清单里没有记录）。 */
	NeverImported,
	/** 配置内容自上次导入后变了（快速判定；可能只是格式变化，深度检查才能确认）。 */
	ConfigChanged,
	/** 资产文件自上次导入后被改过（编辑器里手工改了；可能需要导出回配置）。 */
	AssetChanged,
	/** 配置指向的资产不存在。 */
	AssetMissing,
	/** 深度检查确认：导入会改变资产。 */
	Pending,
	/** 配置解析失败。 */
	ParseError,
	/** 别的工具的配置（有 `kind`，但不在本插件的登记表里）。 */
	Foreign,
	/** 有资产、没有配置（可纳管）。 */
	Unmanaged,
	/** 配置没写目标资产，无从判断。 */
	NoAsset,

	Count,
};

namespace ForgeState
{
	/** 这个状态下「导入」有意义（会建出或改动资产）。 */
	ASSETFORGE_API bool NeedsImport(EForgeSyncState InState);

	/** 快速判定拿不准、值得做一次深度检查。 */
	ASSETFORGE_API bool NeedsDeepCheck(EForgeSyncState InState);

	/** 是本插件管辖的配置（能导入 / 导出 / 预览）。 */
	ASSETFORGE_API bool IsManagedConfig(EForgeSyncState InState);

	/** 界面短标签与说明。 */
	ASSETFORGE_API FText GetLabel(EForgeSyncState InState);
	ASSETFORGE_API FText GetDescription(EForgeSyncState InState);

	/** 机器可读名（面板操作 / 命令行 JSON 用）。 */
	ASSETFORGE_API const TCHAR* ToId(EForgeSyncState InState);
	ASSETFORGE_API EForgeSyncState FromId(const FString& InId);

	// ---- 清单（`Saved/AssetForge/state.json`） ----

	/** 清单文件路径。 */
	ASSETFORGE_API FString GetManifestPath();

	/** 清单键：相对配置根的路径（清单入库，绝对路径在别人机器上全变）。 */
	ASSETFORGE_API FString MakeManifestKey(const FString& InConfigPath);

	/**
	 * 丢掉内存缓存，下次访问重读清单。
	 * 正常不用调：缓存按清单文件修改时间与配置根自动失效。
	 */
	ASSETFORGE_API void InvalidateCache();

	/** 成功导入后记一笔（内容哈希、资产路径、资产文件修改时间）。零动作也要记——那正是「已同步」。 */
	ASSETFORGE_API void RecordImport(const FForgeDocument& InDocument);

	/**
	 * 资产 → 它对应的配置文件（磁盘绝对路径；空 = 没有）。
	 * 先查清单的反向索引（配置可能被挪到非默认目录），再退回默认路径；**只有文件存在才算数**。
	 */
	ASSETFORGE_API FString FindConfigForAsset(const FString& InAssetPath, const FString& InKind);

	/** 清单里所有记录过的资产路径（「是否已纳管」的一部分依据）。 */
	ASSETFORGE_API TSet<FString> GetRecordedAssetPaths();

	// ---- 判定 ----

	/** 快速判定：不加载资产。`InAssetPath` 为空时返回 `NoAsset`。 */
	ASSETFORGE_API EForgeSyncState QuickState(const FString& InConfigPath, const FString& InAssetPath);

	/**
	 * 深度判定：跑一次 dry-run。`OutReport` 非空时回填那份计划（forge.plan 顺手返回，不必再算一遍）。
	 * 结论为「已同步」时刷新清单记录。
	 */
	ASSETFORGE_API EForgeSyncState DeepCheck(const FString& InConfigPath, const FString& InAssetPath, FForgeReport* OutReport = nullptr);

	/**
	 * 变更戳：配置文件与资产文件的修改时间拼成的一串。任一边被写过它就变——
	 * 界面用它判断缓存的计划还能不能用（不必每次选中都重跑 dry-run）。
	 */
	ASSETFORGE_API FString MakeChangeStamp(const FString& InConfigPath, const FString& InAssetPath);

	/** 包名 → 对象路径（`/Game/A/B` → `/Game/A/B.B`）；已经是对象路径的原样返回。`LoadObject` 用它，不依赖引擎对包名的容错。 */
	ASSETFORGE_API FString ToObjectPath(const FString& InAssetPath);

	/** 把资产路径归一化成包名（`/Game/A/B.B` → `/Game/A/B`），配置里两种写法都可能出现。 */
	ASSETFORGE_API FString NormalizeAssetPath(const FString& InAssetPath);
}
