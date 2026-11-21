// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Script/GameTestScriptOptions.h"
#include "Core/Script/GameTestValue.h"
#include "Math/RandomStream.h"

class FGameTestLogTap;
class FJsonObject;
class UObject;
class UWidget;
class UWorld;


/**
 * 一层脚本的作用域：主场景一层，每个 `call` 进去的子脚本一层（子脚本的 vars 独立，协议第 2 节）。
 */
struct GAMETEST_API FGameTestScriptScope
{
	/** 脚本相对场景根的路径（主场景可能为空：临时步骤）。 */
	FString Script;
	TMap<FString, FGameTestValue> Params;
	TMap<FString, FGameTestValue> Vars;
	/** `objects` 段：名字 → 定位器（第 7 节），按需解析。 */
	TMap<FString, TSharedPtr<FJsonObject>> ObjectDefs;
	/** `objects` 段里写成来源字符串的（`"@pawn"`、`"@luaMod.X"`，2.1.0 起）：名字 → 来源，用的时候现取。 */
	TMap<FString, FString> ObjectSources;
	/** 已经找到的对象：同一次运行里同一个名字不重复找；对象销毁后弱引用失效，下次重找。 */
	TMap<FString, TWeakObjectPtr<UObject>> ObjectCache;
	/**
	 * 块里的局部名字（循环的 `$index`、`$item`、`as` 起的名字），内层盖外层。
	 * 不放进 Vars：出了循环就该看不到，嵌套循环各有各的 `$index`。
	 */
	TArray<TMap<FString, FGameTestValue>> Locals;
	/** `return` 带回来的值（被 call 时存进 saveAs）。 */
	FGameTestValue ReturnValue;

	/** 收一份 `objects` 段：对象值进 ObjectDefs，`@` 开头的字符串进 ObjectSources，别的不认（写法检查会报）。 */
	void AddObjects(const FJsonObject& InObjects);

	/** 名字在 objects 里（两种写法都算）。 */
	bool HasObject(const FString& InName) const { return ObjectDefs.Contains(InName) || ObjectSources.Contains(InName); }
};

/**
 * 脚本运行时的上下文（协议第 14 节）：块、判断、取值来源、表达式、接口都显式拿它，不读全局变量。
 *
 * 生命周期：一次 run 一个，由执行器持有；`gt.resolve` / `gt.validate` 这种只看一眼的调用临时造一个。
 * 只在游戏线程上用。
 */
class GAMETEST_API FGameTestFlowContext
{
public:
	FGameTestFlowContext();

	//~ 环境 ---------------------------------------------------------------
	/** 当前世界（PIE 或游戏）；没有游戏在跑时为空，扩展点要判空并返回 Unresolved。 */
	UWorld* World = nullptr;
	/** 多客户端时这一步对着哪个客户端（0 = 第一个）。 */
	int32 Client = 0;
	FGameTestScriptOptions Options;
	/** 名称句柄表（`@句柄`），子系统持有。 */
	TMap<FString, TWeakObjectPtr<UWidget>>* Handles = nullptr;
	/** 场景根（绝对路径），`call`、`_api.json`、`_vars.json` 都相对它。 */
	FString ScenarioRoot;
	/** 本次 run 的产物目录。 */
	FString OutputDir;
	/** 当前时刻（`FPlatformTime::Seconds()`），执行器每帧更新。 */
	double NowSeconds = 0.0;
	/** run 开始的时刻和那时的日志序号：`Log.*` 的 `since` 省略时从这里算。 */
	double RunStartedAt = 0.0;
	uint64 RunLogSeq = 0;
	/** run 开始时 Lua 报错的序号（GameTestLuaBridge::ErrorSeq）：`Log.Errors`、`Lua.Errors` 的 `since` 省略时从这里算。 */
	uint64 RunLuaErrorSeq = 0;
	FGameTestLogTap* LogTap = nullptr;
	/** `Random.Seed` 改它；脚本里所有随机取值都从这里取，结果可复现。 */
	FRandomStream Random;

	/**
	 * 这一步的结果附加信息（`payload.invoke`、`payload.object`、`payload.cond`）：执行器在开步前换成新的，
	 * 扩展点往里写，步骤结束时并进记录。为空时不写（`gt.resolve` 只看值）。
	 */
	TSharedPtr<FJsonObject> StepPayload;

	//~ 作用域 -------------------------------------------------------------
	/** 进一层脚本（主场景、call）。超过 MaxCallDepth 返回 false，不压栈。 */
	bool PushScript(const FString& InScript);
	void PopScript();
	int32 GetScriptDepth() const { return Scopes.Num(); }
	FGameTestScriptScope& GetScope();
	const FGameTestScriptScope& GetScope() const;

	/** 进 / 出一层块局部（循环）。 */
	void PushLocals();
	void PopLocals();
	/** 在最内层块局部里设名字（没有块局部时设进 Vars）。 */
	void SetLocal(const FString& InName, const FGameTestValue& InValue);

	/**
	 * 找 `$名字`：块局部（内到外）→ 变量 → 参数，只看当前这层脚本（子脚本看不到调用方的变量，要用就 `with` 传）。
	 * 找不到返回 false。
	 */
	bool FindVar(const FString& InName, FGameTestValue& OutValue) const;

	/** `set`：已经是块局部的改局部，否则写进变量（没声明的第一次 set 时创建）。 */
	void SetVar(const FString& InName, const FGameTestValue& InValue);

	/** 当前脚本里能看到的全部名字（调试暂停时给 `vars`、报告 `payload.vars`）。 */
	TSharedPtr<FJsonObject> SnapshotVars(int32 InMaxChars = 4000) const;

	//~ 只读闸门 -----------------------------------------------------------
	/**
	 * 条件、表达式里求值时打开：这时只许调只读的函数和接口（协议第 3 节）。
	 * 扩展点在调可能改游戏的东西之前查 `IsReadOnly()`，是就返回 Unresolved（code = expr_not_readonly）。
	 */
	bool IsReadOnly() const { return ReadOnlyDepth > 0; }

	struct GAMETEST_API FReadOnlyScope
	{
		explicit FReadOnlyScope(FGameTestFlowContext& InContext);
		~FReadOnlyScope();

	private:
		FGameTestFlowContext& Context;
	};

private:
	TArray<FGameTestScriptScope> Scopes;
	int32 ReadOnlyDepth = 0;
};
