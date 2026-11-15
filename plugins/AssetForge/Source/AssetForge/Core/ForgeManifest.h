// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * 运行清单：记录每个配置文件的内容哈希、它产出的资产、以及导入时资产文件的修改时间。
 *
 * 放在 `Saved/AssetForge/state.json`（本机状态，不进版本管理），供「待更新」判定使用：配置哈希与资产修改时间
 * 都没动 → 这份配置与资产仍是导入时的一致状态，直接算同步；任一有变化 → 由 dry-run 精判
 * （漂移检测：哈希没变、资产却与上次导入结果不符，说明有人在编辑器里手工改过）。
 */
namespace ForgeManifest
{
	struct ASSETFORGE_API FManifestEntry
	{
		FString ConfigPath;
		FString ContentHash;
		FString AssetPath;
		FString ImportedAtUtc;
		/**
		 * 导入（保存）完成时资产文件的修改时间（ISO8601 文本）。
		 * 「待更新」判定的快路径依据：与配置哈希都没变 → 资产自那次导入后没被写过。
		 * 老条目为空——空表示没有依据，判定对这份配置退回 dry-run 精判（慢但准确）。
		 */
		FString AssetMTimeUtc;
	};

	ASSETFORGE_API bool Load(
		const FString& InManifestPath,
		TMap<FString, FManifestEntry>& OutEntries,
		FString& OutError);

	ASSETFORGE_API bool Save(
		const FString& InManifestPath,
		const TMap<FString, FManifestEntry>& InEntries,
		FString& OutError);
}
