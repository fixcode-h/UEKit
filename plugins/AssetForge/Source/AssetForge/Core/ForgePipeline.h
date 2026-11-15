// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 文档级编排：读 / 写配置文件、按 kind 分派导入 / 导出、报告序列化。
 *
 * 这一层**不知道具体有哪些 kind**——分派读的是 `ForgeKindRegistry`；也**不记清单**——
 * 「一次用户操作」的语义（记清单、批量、落点选择）在 `ForgeService`。
 * 面板操作、命令行、右键菜单都应该调 Service，而不是直接调这里。
 */
namespace ForgePipeline
{
	/** 配置根目录（磁盘绝对路径）：来自项目设置 `UAssetForgeSettings::ConfigRoot`，默认跟着项目的文档根（`ProjectDoc/docs/AssetForge` 或 `docs/AssetForge`）。 */
	ASSETFORGE_API FString GetConfigRootDir();

	/** 某个 kind 的配置子目录，如 `umg-widget` → `<配置根>/umg`。 */
	ASSETFORGE_API FString GetKindConfigDir(const FString& InKind);

	/** 由资产路径推出默认配置路径：`/Game/UI/WBP_X` → `<配置根>/umg/UI/WBP_X.json`。 */
	ASSETFORGE_API FString MakeDefaultConfigPath(const FString& InAssetPath, const FString& InKind);

	/**
	 * 把调用方给的配置路径还原成磁盘绝对路径。
	 * 相对路径先按项目根解释，再按配置根解释（commandlet 与面板调用方的工作目录各不相同）。
	 */
	ASSETFORGE_API FString ResolveConfigPath(const FString& InConfigPath);

	/** 读配置文件 → 文档（含注释剥离、字段校验、schema 版本门禁与 ref 解析）。 */
	ASSETFORGE_API bool LoadDocument(
		const FString& InConfigPath,
		FForgeDocument& OutDocument,
		FForgeReport& OutReport);

	/**
	 * 同上，但配置内容由调用方给（面板预览还没存盘的改动、和 git 里旧版本比较）。
	 * `InConfigPath` 仍然要给：诊断里显示它，配置里的相对 ref 也按它所在的目录找。
	 */
	ASSETFORGE_API bool LoadDocumentFromText(
		const FString& InText,
		const FString& InConfigPath,
		FForgeDocument& OutDocument,
		FForgeReport& OutReport);

	/** 把文档写回配置文件。 */
	ASSETFORGE_API bool SaveDocument(
		const FString& InConfigPath,
		const FForgeDocument& InDocument,
		FForgeReport& OutReport);

	/**
	 * 按 kind 分派导入（真跑时附带资产健康检查）。会就地归一化文档，所以不是 const。
	 * 返回值与 `OutReport.bSucceeded` 一致：处理器成功且报告里没有错误。
	 */
	ASSETFORGE_API bool Import(
		FForgeDocument& InDocument,
		bool bDryRun,
		FForgeReport& OutReport);

	/** 按资产类型分派导出（类型判定读登记表）。返回值与 `OutReport.bSucceeded` 一致。 */
	ASSETFORGE_API bool Export(
		const FString& InAssetPath,
		FForgeDocument& OutDocument,
		FForgeReport& OutReport);

	/** 报告 → JSON。面板操作与命令行 `-json` 共用这一份表示。 */
	ASSETFORGE_API TSharedPtr<FJsonObject> ReportToJson(const FForgeReport& InReport);

	/** 报告 → 人能读的多行文本，供命令行直接打印。 */
	ASSETFORGE_API FString ReportToText(const FForgeReport& InReport);

	/** 动作 / 级别的机器可读名（`create` / `error` …）。 */
	ASSETFORGE_API const TCHAR* ActionToId(EForgeAction InAction);
	ASSETFORGE_API const TCHAR* SeverityToId(EForgeSeverity InSeverity);
}
