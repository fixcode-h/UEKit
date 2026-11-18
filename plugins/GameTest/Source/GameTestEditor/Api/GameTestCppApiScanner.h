// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

struct FGameTestCppApiScanResult
{
	int32 Items = 0;
	int32 Rejected = 0;
	/** `_api.json` 的内容变了（演练时表示「会变」）。 */
	bool bChanged = false;
	/** 写到哪（项目相对路径）。 */
	FString Path;
	TSharedPtr<FJsonObject> Section;
};

/**
 * C++ 项目接口扫描（协议第 8.2 节）：`_settings.json` 的 `api.cpp` 列的文件夹里，
 * `UBlueprintFunctionLibrary` 子类的 static + BlueprintCallable / BlueprintPure 函数。
 *
 * 只在编辑器里做：要读 UHT 留下的源码元数据（ModuleRelativePath、ToolTip、Category、DisplayName），
 * 打包的游戏里没有。结果写进 `_api.json` 的 cpp 段，运行时只读它。
 */
class GameTestCppApiScanner
{
public:
	/** `api.cpp` 没写时的默认文件夹。 */
	static TArray<FString> DefaultFolders();

	/** 读 `_settings.json` 的 `api.cpp`（相对项目根，可带 `**`）。没写、读不了时用默认值。 */
	static TArray<FString> ReadFolders();

	/** 扫描并（不是演练时）写 `_api.json`。 */
	static bool ScanAndWrite(bool bInDryRun, FGameTestCppApiScanResult& OutResult, FString& OutError);

	/** 只扫描，返回 cpp 段（`{scannedAt, folders, items, rejected}`）。 */
	static TSharedRef<FJsonObject> Scan(const TArray<FString>& InFolders);

	/** 项目相对路径是否落在这些文件夹里（`**` 表示任意层，含零层）。 */
	static bool InFolders(const FString& InProjectRelative, const TArray<FString>& InFolders);

	/** `UQuestTestLibrary` → `QuestTest`：去掉 U 前缀和 Library / FunctionLibrary 这类后缀。 */
	static FString ShortClassName(const FString& InClassName);
};
