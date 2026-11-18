// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Script/GameTestScriptOptions.h"

class FJsonObject;
class FJsonValue;
struct FGameTestGuardDef;

/**
 * 一条「条目怎么认」规则（1.4.0）：录制时点到这类条目，直接按这几个字段写标识，不再问人。
 * 录制暂停询问时勾「以后都按这个认」写进来，面板运行设置里能改、删。
 */
struct GAMETEST_API FGameTestKeyRule
{
	/** 条目的类名（`WBP_BagSlot`，带不带 `_C` 都行）。 */
	FString Entry;
	/** 只在这个容器里用（选择器，如 `WBP_Bag:Root/ItemList`）；空表示这类条目在哪都按它认。 */
	FString Container;
	/** 条件左边（`Data.ItemId`、`@lua.cfgId`），几个都满足才算。 */
	TArray<FString> By;
};

/**
 * 项目级的运行配置：场景根下的 `_settings.json`，跟着项目文档走（和场景一样进版本管理、全组共用），
 * 不放 UE 的 Config —— 面板 GameTest 页「运行设置」读写它（`gt.config`）。
 *
 * ```json
 * { "uiWaitTimeoutMs": 0, "uiStepGapMs": 1000, "clickVia": "code", "randomSeed": 42,
 *   "setup": [ { "action": "console", "command": "gm.ResetGuide" } ],
 *   "move": { "fallback": "none", "teleportCommand": "gm.Goto {x} {y} {z}", "hotkeys": true },
 *   "guards": [ { "id": "b1", "when": { "target": "WBP_Notice:.", "onScreen": true },
 *                 "do": [ { "action": "widget.close", "target": "WBP_Notice:." } ] } ],
 *   "allowLuaInvoke": true, "allowLuaInCond": false, "maxCallDepth": 8,
 *   "itemKeys": [ { "entry": "WBP_BagSlot", "container": "WBP_Bag:Root/ItemList", "by": ["Data.ItemId"] } ],
 *   "skipIf": { "enabled": true, "settleMs": 300, "verify": true, "verifyTimeoutMs": 5000 } }
 * ```
 *
 * 1.9.0 起 `blockers`（阻挡界面）换成了 `guards`（守护，见 docs/game-test-script.md 第 5 节）：读到只有 blockers 的旧文件时
 * 在内存里改写成 guards，Load 顺手把文件改写掉（原文件存 .bak）。
 * 面板和别的工具往这个文件里加的字段（`api`、`objectKeys`…）原样保留：ToJson 从读到的原文出发，只改认得的字段。
 */
struct GAMETEST_API FGameTestRunConfig
{
	/** UI 等待上限（毫秒，0 = 不限）；-1 表示没写，按项目设置。 */
	int32 UiWaitTimeoutMs = -1;
	/**
	 * 相邻两个 UI 操作之间至少隔多久（毫秒，0 = 不隔）；-1 表示没写，按默认 1000。
	 * 和录制时的手速无关：录制的场景 stepDelayMs 写 0，真人点得快时回放会快到界面还没反应过来。
	 */
	int32 UiStepGapMs = -1;
	/** click 没写 via 时的点击方式：code / pointer；空表示默认（code）。 */
	FString ClickVia;
	/** 全局守护（`guards` 原文，已校验）；没写时为空。 */
	TSharedPtr<FJsonValue> Guards;
	/** 解析好的全局守护（开跑时直接用）。 */
	TArray<TSharedRef<FGameTestGuardDef>> ParsedGuards;
	/** allowLuaInvoke / allowLuaInCond / maxCallDepth。 */
	FGameTestScriptOptions ScriptOptions;
	/** 读到的原文：写回时保留不认识的字段。 */
	TSharedPtr<FJsonObject> Raw;
	/** 这次读的时候把 blockers 改写成了 guards（Load 据此改写文件）；每条怎么改的记在 MigrationNotes。 */
	bool bMigratedBlockers = false;
	TArray<FString> MigrationNotes;
	/** 前置步骤：从场景文件跑时先跑这几步（重置存档、清引导之类的 GM），写法和场景里的步骤一样。 */
	TArray<TSharedPtr<FJsonObject>> Setup;
	/** 每次开跑前用它初始化引擎的全局随机；没写就不固定。 */
	TOptional<int32> RandomSeed;
	/**
	 * move 步骤的默认设置和录制快捷键（`fallback`、`teleportCommand`、`radius`、`drive`、`forwardKey`、`hotkeys`），
	 * 原样存成 JSON（校验见 GameTestMove::ValidateDefaults）；没写时为空。
	 */
	TSharedPtr<FJsonObject> Move;
	/** 条目怎么认（`itemKeys`）。 */
	TArray<FGameTestKeyRule> ItemKeys;
	/**
	 * 已达成就跳过（1.6.0）：`skipIf` 段原样存（`false` 或 `{enabled, settleMs, verify, verifyTimeoutMs}`，
	 * 校验见 GameTestSkipIf::ApplyOptions）；没写时为空，按默认（开、300ms、做完核对 5 秒）。
	 */
	TSharedPtr<FJsonValue> SkipIf;
};

class GAMETEST_API GameTestRunConfig
{
public:
	/** 配置文件名：放在场景根下，列场景时跳过它。 */
	static const TCHAR* FileName();

	/** 配置文件的绝对路径。 */
	static FString GetPath();

	/** 读配置。文件不存在算成功（空配置）；格式错返回 false 并写清楚哪里错。 */
	static bool Load(FGameTestRunConfig& OutConfig, FString& OutError);

	/** 从 JSON 解析并校验（gt.config 写入前、Load 都走这一条）。 */
	static bool FromJson(const FJsonObject& InJson, FGameTestRunConfig& OutConfig, FString& OutError);

	static TSharedPtr<FJsonObject> ToJson(const FGameTestRunConfig& InConfig);

	/** 校验后写盘（格式化的 JSON，便于在版本管理里看差异）。 */
	static bool Save(const FGameTestRunConfig& InConfig, FString& OutError);

	/** 认得的字段（写回时以配置为准；其余字段原样保留）。 */
	static const TArray<FString>& KnownFields();

	/** gt.config 整份写入时：`InExisting`（盘上的）里有、`InOutIncoming` 里没有的不认得的字段补回去，免得面板不认识的段被冲掉。 */
	static void PreserveUnknown(FJsonObject& InOutIncoming, const FJsonObject& InExisting);

	/** 全局守护里出现的界面名（录制时推「连带跳过」要排除它们：守护会处理的界面不算步骤打开的）。 */
	static TArray<FString> GuardScreens(const FGameTestRunConfig& InConfig);

	/** 没写 uiStepGapMs 时的 UI 操作间隔（毫秒）。 */
	static constexpr int32 DefaultUiStepGapMs = 1000;

	/** uiStepGapMs 的上限（毫秒）：再长就该写 wait 了。 */
	static constexpr int32 MaxUiStepGapMs = 60000;
};
