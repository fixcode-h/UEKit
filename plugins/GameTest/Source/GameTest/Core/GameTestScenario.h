// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * 场景根目录下的一份场景（列举结果的一项）。
 *
 * 解析失败的场景**也在清单里**，只是 `ParseError` 非空——静默跳过会让"我明明写了这个文件"
 * 变成一个查不出来的问题。窗口与 `gt.scenarios` 用的是同一份扫描结果。
 */
struct GAMETEST_API FGameTestScenarioInfo
{
	FString Name;
	/** 相对场景根目录的路径（`pie/showcase-smoke.json`），也是 `gt.run` 要的值。 */
	FString RelativePath;
	FString AbsolutePath;
	FString Note;
	int32 StepCount = 0;
	/** 作者维护的内容版本；没写为 0。 */
	int32 Revision = 0;
	/** 作者的标签，只读回报（本次不做按标签过滤运行）。 */
	TArray<FString> Tags;
	/** 非空表示这份文件解析或校验没过；此时除路径外的字段都是尽力而为。 */
	FString ParseError;
};

/**
 * 场景文件的顶层校验与取用。
 *
 * 放在算法层是为了能单测：门禁错了的代价是"整份配置写了一晚上却什么都没跑"，
 * 而它本身是纯数据判断，不需要引擎。
 */
class GAMETEST_API GameTestScenario
{
public:
	/** 当前支持的场景格式版本；场景里写更高的版本会被明确拒绝。 */
	/**
	 * 当前支持的场景格式版本；场景里写更高的版本会被明确拒绝。
	 * 2：click / type 默认走真实输入（指针、按键），旧的按控件类的代码级操作要写 `via: "code"`。
	 */
	static constexpr int32 SupportedSchemaVersion = 2;

	/**
	 * 旧版本场景的兼容：schemaVersion 1 的 click / type 当时是代码级操作，没写 via 的补上 `via: "code"`，
	 * 行为和以前一样。只改内存里的步骤，不改文件。
	 */
	static void ApplyLegacyDefaults(const TSharedPtr<FJsonObject>& InJson, TArray<TSharedPtr<FJsonObject>>& InOutSteps);

	/** 场景文件的 kind 取值。 */
	static const TCHAR* GetKind()
	{
		return TEXT("gametest-scenario");
	}

	/**
	 * 校验顶层字段：`kind`、`schemaVersion`、非空 `steps`。
	 * 失败时 OutErrors 是字段级诊断（可多条）。
	 */
	static bool Validate(const TSharedPtr<FJsonObject>& InJson, TArray<FString>& OutErrors);

	/** 取步骤数组（每条是原始 JSON，交给动作表解析）。校验不通过时返回 false。 */
	static bool CollectSteps(
		const TSharedPtr<FJsonObject>& InJson,
		TArray<TSharedPtr<FJsonObject>>& OutSteps,
		TArray<FString>& OutErrors);

	/** 取场景名；没有就退回给定的备选（通常是文件名）。 */
	static FString GetName(const TSharedPtr<FJsonObject>& InJson, const FString& InFallback);

	/** 取 `revision`（作者维护的内容版本）；缺省或不是正数时为 0。 */
	static int32 GetRevision(const TSharedPtr<FJsonObject>& InJson);

	/** 取 `tags`；缺省、类型不对或是空串时返回空数组（元素会去空白、丢空项）。 */
	static TArray<FString> GetTags(const TSharedPtr<FJsonObject>& InJson);

	/**
	 * 递归扫描场景根目录下全部 `.json`，按 `RelativePath` 排序返回。
	 *
	 * 目录不存在时返回空数组（调用方据此给出"场景目录在哪"的提示），不是错误。
	 */
	static TArray<FGameTestScenarioInfo> ScanDirectory(const FString& InRootDir);
};
