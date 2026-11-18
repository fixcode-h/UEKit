// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UObject;
class UWorld;
class FGameTestFlowContext;
struct FGameTestValue;

/**
 * 对象定位器（协议第 7 节）：编写时抓的 UObject 存成规则，运行时按规则重新找。
 *
 * ```json
 * { "class": "BP_ShopNpc_C", "in": "world", "key": { "NpcId": 1001 }, "component": null, "near": null, "client": 0 }
 * ```
 */
struct GAMETEST_API FGameTestObjectLocator
{
	FString Class;
	/** `world`（默认）/ `widgets` / `all` / 来源路径（`@sub.UQuestSubsystem.ActiveQuests`）。 */
	FString In = TEXT("world");
	/** `{路径: 值}`：都要等于。值可以是 `$var`、表达式（定位时按上下文求值）。 */
	TArray<TPair<FString, TSharedPtr<FJsonValue>>> Key;
	/** 在找到的 Actor 上再取组件（类名或组件名）。 */
	FString Component;
	/** `[x, y, z]`、`$var`、`@pawn`：多个时取最近的。 */
	TSharedPtr<FJsonValue> Near;
	/** 多客户端时在哪个世界找（`server` 或序号），空表示步骤的默认世界。 */
	FString Client;

	/** 解析并校验（未知字段、缺 class 都报出来）；场景加载和 gt.validate 用。 */
	static bool Parse(const FJsonObject& InJson, FGameTestObjectLocator& OutLocator, TArray<FString>& OutErrors);
	TSharedRef<FJsonObject> ToJson() const;
	/** 一句话描述（报告、报错里用）：`BP_ShopNpc（NpcId=1001）`。 */
	FString Describe() const;
};

enum class EGameTestLocate : uint8
{
	Found,
	/** 0 个：调用方等到超时。 */
	NotFound,
	/** 有 key 还命中多个：报 object_ambiguous。 */
	Ambiguous,
	/** 定位器本身有问题（类不存在、来源路径错）：等也没用，直接失败。 */
	Error,
};

struct GAMETEST_API FGameTestLocateResult
{
	UObject* Picked = nullptr;
	/** 没 key、多个候选时取了第一个（报告标 byFirst）。 */
	bool bByFirst = false;
	/** 按什么挑的：`only` / `key` / `near` / `first`。 */
	FString By;
	int32 Candidates = 0;
	/** 失败时的说明（候选和它们的标识值）。 */
	FString Message;

	/** 报告的 `payload.object`。 */
	TSharedRef<FJsonObject> ToJson(const FString& InName, const FGameTestObjectLocator& InLocator) const;
};

class GAMETEST_API GameTestObjectLocate
{
public:
	/**
	 * 按定位器找一次（不等待）。key 的值、near 里的 `$var` / 表达式按上下文求值；
	 * `in` 是来源路径时按来源取（容器属性里的对象）。
	 */
	static EGameTestLocate Locate(const FGameTestObjectLocator& InLocator, FGameTestFlowContext& InContext, FGameTestLocateResult& OutResult);

	/** 全部候选（Object.FindAll、Object.Count 用）：已按 key 过滤、按固定顺序排好。 */
	static bool FindAll(const FGameTestObjectLocator& InLocator, FGameTestFlowContext& InContext, TArray<UObject*>& OutObjects, FString& OutError);

	/**
	 * `objects` 里的名字（`{"$obj": "名字"}`、`@obj.名字`）→ 对象：先看这次运行的缓存（对象还活着就直接用），
	 * 再按定位器找。找不到 Unresolved（object_not_found，调用方按步骤超时等）、多个 object_ambiguous；
	 * 找到时往 `StepPayload` 写 `payload.object`。
	 */
	static FGameTestValue ResolveNamed(const FString& InName, FGameTestFlowContext& InContext);

	/**
	 * 读对象上一条路径的值（定位器 key 用）：`@lua.x` 读绑的 Lua 实例表，`@self.x` 或不带来源走成员访问（反射优先）。
	 */
	static FGameTestValue ReadKey(UObject* InObject, const FString& InPath, FGameTestFlowContext& InContext);
};
