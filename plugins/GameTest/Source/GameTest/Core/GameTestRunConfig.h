// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/** 阻挡界面出现时怎么处理。 */
enum class EGameTestBlockerAction : uint8
{
	/** 从界面上移除（RemoveFromParent，移不掉就折叠）。 */
	Close,
	/** 调这个界面上的一个无参函数（例如它自己的「关闭」）。 */
	Call,
	/** 执行一条控制台指令（GM、跳过引导之类）。 */
	Command,
};

/**
 * 一条阻挡界面规则：跑测试时这个 WBP 一出现就按 Action 处理掉，不让它挡住后面的步骤
 * （公告、引导、断线重连、活动弹窗这类不确定什么时候弹的界面）。
 */
struct GAMETEST_API FGameTestBlockerRule
{
	/** 界面的类名（`WBP_Notice`，带不带 `_C` 都行）或资产路径（`/Game/UI/WBP_Notice`）；子类也算。 */
	FString Widget;
	EGameTestBlockerAction Action = EGameTestBlockerAction::Close;
	/** Action=Call 时调的函数名。 */
	FString Function;
	/** Action=Command 时执行的指令。 */
	FString Command;
	/** 给人看的说明。 */
	FString Note;
	/** 出现后过多久再处理（毫秒）：有的界面要等打开动画播完，处理早了它自己又开回来。 */
	int32 DelayMs = 0;
	bool bEnabled = true;
};

/**
 * 项目级的运行配置：场景根下的 `_settings.json`，跟着项目文档走（和场景一样进版本管理、全组共用），
 * 不放 UE 的 Config —— 面板 GameTest 页「运行设置」读写它（`gt.config`）。
 *
 * ```json
 * { "uiWaitTimeoutMs": 0, "uiStepGapMs": 1000, "clickVia": "code", "randomSeed": 42,
 *   "setup": [ { "action": "console", "command": "gm.ResetGuide" } ],
 *   "move": { "fallback": "none", "teleportCommand": "gm.Goto {x} {y} {z}", "hotkeys": true },
 *   "blockers": [ { "widget": "WBP_Notice", "action": "close" },
 *                 { "widget": "WBP_Guide", "action": "call", "function": "Skip" },
 *                 { "widget": "WBP_Reconnect", "action": "command", "command": "gm.Reconnect" } ] }
 * ```
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
	TArray<FGameTestBlockerRule> Blockers;
	/** 前置步骤：从场景文件跑时先跑这几步（重置存档、清引导之类的 GM），写法和场景里的步骤一样。 */
	TArray<TSharedPtr<FJsonObject>> Setup;
	/** 每次开跑前用它初始化引擎的全局随机；没写就不固定。 */
	TOptional<int32> RandomSeed;
	/**
	 * move 步骤的默认设置和录制快捷键（`fallback`、`teleportCommand`、`radius`、`drive`、`forwardKey`、`hotkeys`），
	 * 原样存成 JSON（校验见 GameTestMove::ValidateDefaults）；没写时为空。
	 */
	TSharedPtr<FJsonObject> Move;
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

	static const TCHAR* ActionName(EGameTestBlockerAction InAction);

	/** 没写 uiStepGapMs 时的 UI 操作间隔（毫秒）。 */
	static constexpr int32 DefaultUiStepGapMs = 1000;

	/** uiStepGapMs 的上限（毫秒）：再长就该写 wait 了。 */
	static constexpr int32 MaxUiStepGapMs = 60000;
};
