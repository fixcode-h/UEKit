// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Core/ForgeTypes.h"

/**
 * 配置目录的约定判定（生成物、噪声路径、配置分类、导出落点），无 IO 的部分都可直接单测。
 *
 * 与 `Core/` 的分工：`Core/` 是「与资产类型、界面都无关」的管道，它不认识
 * `_schema/` 这类目录约定；本文件承载的正是那些约定。`ForgeService` 是它的主要调用方。
 */
namespace ForgeEditorUtils
{
	/**
	 * 名字（单个文件名或目录名）是不是插件的生成物——以 `_` 或 `.` 开头（`.schema/`；`.`、`..` 除外）。
	 *
	 * 用**形状**而不是名字清单：`_schema/`、`_forge-state.json` 都是插件自己写的，
	 * 将来再加一个生成物也不必改这里。历史上三处各写各的（`StartsWith("_")` /
	 * `Contains("_schema")` 两处），后两处会漏掉 `_forge-state.json`，还会误杀路径里
	 * 恰好含 `_schema` 的正常配置——那个误杀更隐蔽，因为它只在命名凑巧时发作。
	 */
	ASSETFORGE_API bool IsGeneratedEntry(const FString& InName);

	/** 完整路径的任一段是不是生成物（`.../_schema/x.json`、`.../_forge-state.json`）。 */
	ASSETFORGE_API bool IsGeneratedPath(const FString& InPath);

	/**
	 * 资产路径是不是插件的中间产物（外部 Actor、开发者目录），不该出现在「未纳管资产」列表里。
	 *
	 * `__External` 与 `/Developers/` 由引擎生成，不是可纳管的资产。
	 */
	ASSETFORGE_API bool IsNoiseAssetPath(const FString& InPackagePath);

	/** 一份配置文件的分类。 */
	enum class EForgeConfigClass : uint8
	{
		/** 本插件支持的配置，可进同步判定。 */
		Parsed,
		/** 有 `kind` 但不属于本插件支持的类型——它没坏，只是不归这里管。 */
		ForeignKind,
		/** 解析失败（语法错误、缺字段、版本不支持）。 */
		ParseFailed,
	};

	/** 分类结果：三种情况各自只填自己那几个字段。 */
	struct ASSETFORGE_API FForgeConfigClassification
	{
		EForgeConfigClass Class = EForgeConfigClass::ParseFailed;

		/** `Parsed` 时有值。 */
		FString Kind;
		/** 展开过的片段文件（include，绝对路径）。 */
		TArray<FString> Fragments;
		FString AssetPath;
		FString ParentClass;
		int32 Revision = 0;

		/** `ForeignKind` 时有值。 */
		FString ForeignKind;

		/** `ParseFailed` 时有值：为什么没解析成（可能为空，如文件读不出来）。 */
		FString ErrorMessage;
	};

	/**
	 * 从「文档是否加载成功 + 原始 JSON」给出分类，**无 IO**，因此测试可以直接构造入参。
	 *
	 * 判定顺序照搬原来管理窗口的行为：先看文档加载结果；没成功再看原始 JSON 里的 `kind` 是不是
	 * 别人的种类（这时才算 `ForeignKind`）；都不是就是 `ParseFailed`。
	 * `InSupportedKinds` 由调用方给（而不是内部查管道），这样测试能自带一份名单。
	 */
	ASSETFORGE_API FForgeConfigClassification ClassifyConfig(bool bInParsed, const FForgeDocument& InDocument, const FForgeReport& InReport, const TSharedPtr<FJsonObject>& InRawJson, const TArray<FString>& InSupportedKinds);

	/** 读一份配置文件再分类（`ForgeService::DescribeConfig` 用）。 */
	ASSETFORGE_API FForgeConfigClassification ClassifyConfigAtPath(const FString& InAbsolutePath);

	/**
	 * 导出方向的落点：清单里有既有配置就用它，没有才退回默认路径。
	 *
	 * 唯一调用方是 `ForgeService::ExportAsset`（所有导出入口都经它），
	 * 否则手工挪过目录的配置会被某个入口按默认路径再建一份。
	 */
	ASSETFORGE_API FString ChooseExportConfigPath(const FString& InExistingPath, const FString& InDefaultPath);

}
