// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

/** `_api.json` 里的一个项目接口（C++ 蓝图函数库的静态函数，或 Lua 模块上的函数）。 */
struct GAMETEST_API FGameTestProjectApi
{
	/** `项目.QuestTest.AcceptQuest`：脚本里按它调。 */
	FString Name;
	/** `cpp` / `lua` */
	FString Src;
	/** C++：类路径（`/Script/Demo.QuestTestLibrary`）。 */
	FString Class;
	/** Lua：require 路径（`GameTestApi.QuestApi`）。 */
	FString Module;
	FString Function;
	FString Category;
	FString Display;
	FString Note;
	bool bReadOnly = false;
	FString File;
	int32 Line = 0;
	/** 参数表原样（`[{name, type, …}]`），列接口、校验参数名时用。 */
	TArray<TSharedPtr<FJsonValue>> Args;
	FString Returns;
};

/**
 * 场景根的 `_api.json`（协议第 8.2 节）：项目接口的清单，按 `src` 分段，谁扫谁写自己那一段。
 * C++ 段由 GameTestEditor 扫描写，Lua 段由面板扫描写；运行时（含打包的游戏）只读，按类 + 函数名反射调。
 */
class GAMETEST_API GameTestApiFile
{
public:
	static const TCHAR* FileName();
	static FString GetPath();

	/** 读全部接口和改名表。文件不存在算成功（没有项目接口）；格式错返回 false 并写清楚。 */
	static bool Load(TArray<FGameTestProjectApi>& OutItems, TMap<FString, FString>& OutRenamed, FString& OutError);

	/** 某一段（`cpp` / `lua`）扫描时被拒的函数（`gt.api` 给面板显示「为什么没认」）。没有文件或没有这段时为空。 */
	static TArray<TSharedPtr<FJsonValue>> LoadRejected(const FString& InSrc);

	/**
	 * 换掉一段（`cpp` / `lua`），别的段和 `renamed` 原样保留。内容和原来一样（不比 `scannedAt`）时不写盘，
	 * 免得每次启动编辑器都在版本库里留一个只改了时间的差异。bDryRun 只比较不落盘。
	 */
	static bool WriteSection(const FString& InSrc, const TSharedRef<FJsonObject>& InSection, bool bInDryRun, bool& bOutChanged, FString& OutError);

	/** 文件内容变了的通知（运行时据此重载项目接口）。 */
	static FSimpleMulticastDelegate& OnChanged();
};
