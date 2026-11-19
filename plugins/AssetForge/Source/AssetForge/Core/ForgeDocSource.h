// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 文档来源层：配置文件 ↔ 一份完整的文档 JSON，中间处理 include。
 *
 * 一个资源还是一个主文件，但某一段（`"animations": { "include": "x.json" }`）或某个子节点
 * （`children` 里的 `{ "include": "x.json" }`）可以放在片段文件里。片段文件顶层写 `fragment`
 * （`node` 或段名）和同名字段，不许写 `kind`——两者互斥，扫描配置根时片段永远不当资源。
 *
 * 读（`Expand`）：先把 include 原样展开成和单文件一样的文档，后面的解析、演练、差异、导入、哈希全照旧；
 * 写（`Collapse`）：把整份新文档按原来的 include 结构拆回各文件，片段的内容回片段文件，新增的写主文件。
 * include 只在这一层认，别处都看不见它（和生成子 WBP 的 `ref` 不同：include 展开后还是同一个资产）。
 *
 * 读文件经调用方给的加载器：真跑用 `DiskLoader()`，单测给一张内存里的「路径 → JSON」表。
 */
namespace ForgeDocSource
{
	/** 读一个 JSON 文件（绝对路径）。失败时 OutError 写原因。 */
	using FLoader = TFunction<bool(const FString&, TSharedPtr<FJsonObject>&, FString&)>;

	/** 从磁盘读（剥注释）。 */
	ASSETFORGE_API FLoader DiskLoader();

	/** 展开结果。 */
	struct FExpanded
	{
		/** 展开后的整份文档。 */
		TSharedPtr<FJsonObject> Json;

		/** 用到的片段文件（绝对路径，去重，按第一次出现的顺序）。 */
		TArray<FString> Fragments;

		/** 展开后的字段路径前缀（`animations`、`root.children[1]`）→ 来自的片段文件（相对主文件所在目录）。诊断加文件名用。 */
		TMap<FString, FString> Origins;
	};

	/** 要写的一个文件。 */
	struct FFileWrite
	{
		FString Path;
		TSharedPtr<FJsonObject> Json;
		/** 片段文件（写回时 `$schema` 指向片段的 schema）。 */
		bool bFragment = false;
	};

	/** include 写的路径 → 绝对路径（相对引用它的文件所在目录；正斜杠、折叠过 `..`）。 */
	ASSETFORGE_API FString ResolveIncludePath(const FString& InInclude, const FString& InFromFile);

	/** 顶层有 `fragment`、没有 `kind`：是片段，不是资源配置。 */
	ASSETFORGE_API bool IsFragment(const TSharedPtr<FJsonObject>& InJson);

	/** 读文件看是不是片段（读不了、不是 JSON 都算不是）。扫描配置根时用它跳过片段。 */
	ASSETFORGE_API bool IsFragmentFile(const FString& InPath);

	/**
	 * 展开主文件里的 include。主文件是片段、或 `kind` 与 `fragment` 同时出现时报错；片段读不了、类型对不上
	 * （动画片段 include 到控件树里）、成环都报错，路径指到 include 所在的位置（展开后的坐标）。
	 * 同一个片段在一份配置里被引用多次给警告。
	 */
	ASSETFORGE_API bool Expand(const TSharedPtr<FJsonObject>& InMain, const FString& InMainPath, const FLoader& InLoader, FExpanded& OutExpanded, TArray<FForgeDiagnostic>& OutDiagnostics);

	/** 从磁盘读主文件再展开。 */
	ASSETFORGE_API bool LoadFile(const FString& InMainPath, FExpanded& OutExpanded, TArray<FForgeDiagnostic>& OutDiagnostics);

	/** 主文件内容由调用方给（面板里还没存盘的内容），片段从磁盘读（相对路径按 InMainPath 找）。 */
	ASSETFORGE_API bool LoadText(const FString& InText, const FString& InMainPath, FExpanded& OutExpanded, TArray<FForgeDiagnostic>& OutDiagnostics);

	/**
	 * 诊断路径加上片段文件名：`WBP_Shop.parts/animations.json: animations[0].tracks[0].channel`。
	 * 只改下标 InStartIndex 起的那些（前面的已经加过）；按最长的前缀认，嵌套片段算最里面那个。
	 */
	ASSETFORGE_API void LabelDiagnostics(const TMap<FString, FString>& InOrigins, TArray<FForgeDiagnostic>& InOutDiagnostics, int32 InStartIndex);

	/**
	 * 写回：把整份新文档按主文件原来的 include 结构拆成要写的文件（第一项总是主文件）。
	 * 原来是 include 的段，新内容写回那个片段；子节点按 name 认回原来的片段，片段里没有的新节点写在主文件。
	 * 主文件还不存在或读不了：整份写主文件。片段读不了：那部分写回引用它的文件并给警告。
	 * 片段成环：整份写主文件并给警告。不碰磁盘，真写由调用方做（`WriteIfChanged`）。
	 */
	ASSETFORGE_API bool Collapse(const TSharedPtr<FJsonObject>& InDocument, const FString& InMainPath, const FLoader& InLoader, TArray<FFileWrite>& OutWrites, TArray<FForgeDiagnostic>& OutDiagnostics);

	/** 内容和磁盘上的等价（规范形态相同）就不写：不动修改时间，P4 不用签出。返回是否成功（没写也算成功）。 */
	ASSETFORGE_API bool WriteIfChanged(const FString& InPath, const TSharedPtr<FJsonObject>& InJson, bool& bOutWritten, FString& OutError);
}
