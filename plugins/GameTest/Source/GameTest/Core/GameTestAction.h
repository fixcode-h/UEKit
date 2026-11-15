// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/GameTestRunner.h"

class FJsonObject;
struct FGameTestStep;

/**
 * 动作词汇表的一行。
 *
 * `Params` 是**动作专属**字段的白名单（逗号分隔）。解析步骤时，任何既不在公共字段、
 * 也不在本动作白名单里的键都是**硬错误**——拼错 `targe` 而静默忽略，正是 AssetForge
 * 那边总结出的最贵的一类缺陷（配置写错了却报"成功"）。
 */
struct FGameTestActionInfo
{
	FName Name;
	const TCHAR* Params = TEXT("");
	bool bNeedsTarget = false;
	bool bEditorOnly = false;
	const TCHAR* Summary = TEXT("");

	/** 实现。登记时必填：说明和实现一起登记，不会出现「表里有、实现里没有」。 */
	FGameTestHandler Handler = nullptr;

	/** 提供它的模块（`GameTest`、`GameTestCommonUI`……），进动作清单，面板据此分组。 */
	const TCHAR* Module = TEXT("GameTest");
};

/**
 * 可选模块提供的动作：模块没加载时，写了这些动作的场景会被告知「要开哪个插件」，
 * 而不是一句「未知动作」。只是提示表，不影响登记。
 */
struct FGameTestOptionalAction
{
	const TCHAR* Name;
	const TCHAR* Module;
	const TCHAR* Plugin;
};

/**
 * 动作登记表：解析、校验、自描述（`gt.actions` 与面板的工具说明共用这一份真源）。
 *
 * 主模块的动作在第一次用到时登记；可选模块（CommonUI / GAS / EnhancedInput）在自己的
 * StartupModule 里 `Register`、ShutdownModule 里 `Unregister`。只在游戏线程上调用。
 */
class GAMETEST_API GameTestActions
{
public:
	/** 已登记的全部动作（主模块的在前，顺序即文档顺序）。 */
	static const TArray<FGameTestActionInfo>& GetAll();

	/** 登记一个动作；同名的会被替换（模块热重载时重新登记）。 */
	static void Register(const FGameTestActionInfo& InInfo);

	static void Unregister(FName InAction);

	/** 动作 → 实现；没登记时返回 nullptr。 */
	static FGameTestHandler FindHandler(FName InAction);

	/** 可选模块的动作和它要的插件（没加载时的提示用）。 */
	static const TArray<FGameTestOptionalAction>& GetOptionalActions();

	/** 公共字段：每个动作都接受。 */
	static const TArray<FString>& GetCommonParams();

	static const FGameTestActionInfo* Find(FName InAction);

	/**
	 * 解析一条步骤 JSON。失败时 `OutErrors` 是字段级诊断（可多条），
	 * 并且在**执行任何动作之前**返回 false。
	 */
	static bool ParseStep(const TSharedPtr<FJsonObject>& InJson, int32 InIndex, FGameTestStep& OutStep, TArray<FString>& OutErrors);

	/** 词汇表 → JSON。 */
	static TSharedPtr<FJsonObject> DescribeAll();

	/** 名字写错时的候选（"你是想写 click 吗"）。 */
	static void Suggest(const FString& InWritten, TArray<FString>& OutSuggestions);
};
