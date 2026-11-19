// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 算法层：配置文档的「段」——顶层字段按固定顺序分组。
 *
 * 一个资源一个 JSON，太长时人和 Agent 都按段读、按段改：头（`$schema`、`//`、kind、schemaVersion、revision、
 * asset、parent 和其余小字段）→ `styles` → `root` → 各 kind 登记的大段（UMG 的 `animations`……）。
 * 段的名单和顺序由 kind 注册表给（`ForgeKindSchema::GetBodySections`），这里只做与 kind 无关的重排和取段。
 */
namespace ForgeSections
{
	/** 头段的名字（`forge.describe section=header`）。 */
	constexpr const TCHAR* HeaderSection = TEXT("header");

	/**
	 * 按段重排顶层字段，返回新对象（字段内容不动、不复制深层）：固定头字段 → 其余小字段（保持原顺序）→
	 * InBodySections 里列的段（按列的顺序）。写回配置前调用：手写顺序不对也能读，写回时被排好。
	 */
	ASSETFORGE_API TSharedPtr<FJsonObject> Reorder(const TSharedPtr<FJsonObject>& InDocument, const TArray<FString>& InBodySections);

	/** 文档里实际有的段（`header` 总在第一个，其后按 InBodySections 的顺序）。 */
	ASSETFORGE_API TArray<FString> ListPresent(const TSharedPtr<FJsonObject>& InDocument, const TArray<FString>& InBodySections);

	/**
	 * 取一段，包成 `{ 段名: 内容 }`（头段直接是头字段组成的对象）：这样取出来的文本本身就是合法 JSON，
	 * 改完交回时也是这个形状。没有这一段返回空。
	 */
	ASSETFORGE_API TSharedPtr<FJsonObject> Extract(const TSharedPtr<FJsonObject>& InDocument, const FString& InSection, const TArray<FString>& InBodySections);
}
