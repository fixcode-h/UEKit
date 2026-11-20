// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Script/GameTestRegistry.h"
#include "Core/Script/GameTestValue.h"

class FGameTestDiagSink;
struct FGameTestExprScope;
class FGameTestFlowContext;
class FJsonObject;

/** 一条判断的结果（协议第 4 节、`gt.resolve conds` 的 parts）。 */
struct GAMETEST_API FGameTestCheckResult
{
	bool bHolds = false;
	/** 实际读到的值（「checked=false」「Gold=120」），成立不成立都填。 */
	FGameTestValue Actual;
	/** 读不到的原因（对象不在、字段不存在）：这时 bHolds 一定是 false。 */
	FString Unresolved;
	/**
	 * 目标所在的屏不在屏上（协议第 4.2 节）：反面判断（`exists:false`、`visible:false`）这时不算数——
	 * 「领取按钮不存在」可能是领过了，也可能是界面还没打开。条件树据此把结论记成「还不能判断」。
	 */
	bool bScreenAbsent = false;
	/** 给人看的一句（「期望 checked=true，实际 false」），进失败信息和报告。 */
	FString Say;
};

/**
 * 一种判断（协议第 4.1 节，2.0.0 起对象判断见 docs/game-test-ref.md 第 4 节）：对象的属性和状态、代码变量、接口、表达式、附近的 Actor、常用名……
 * 一种一个实现，一个文件（`Observe/Checks/GameTestCheck<名字>.cpp`），登记进 `GameTestChecks`。
 *
 * 条件树（all / any / not、稳住）是公共逻辑（GameTestCond），判断只回答「这一条此刻成不成立」。
 * 选哪个实现：按登记顺序问 `Matches`，第一个认的负责——所以更具体的判断先登记（`expr` 先于 `value`）。
 */
class GAMETEST_API IGameTestCheck
{
public:
	virtual ~IGameTestCheck() = default;

	/** `obj`、`value`、`api`、`expr`、`actorNear`、`alias`。 */
	virtual FName GetKey() const = 0;

	virtual FString GetSummary() const = 0;

	/** 这个判断对象归不归它（一般看有没有它的标志字段：`expr`、`api`、`value`……）。 */
	virtual bool Matches(const FJsonObject& InJson) const = 0;

	/** 它认的全部字段（比较字段 equals / gt… 和 `client` 由框架加，不用列）。不在里面的字段报 unknown_field。 */
	virtual const TArray<FString>& GetFields() const = 0;

	/** 写法检查（加载、`gt.validate` 时）：类型、必填、表达式能不能解析。字段白名单框架已经查过。 */
	virtual void Validate(const FJsonObject& InJson, FGameTestDiagSink& InSink) const
	{
	}

	/** 要知道作用域（变量、objects 名字）的写法检查；默认转给 Validate。 */
	virtual void ValidateIn(const FJsonObject& InJson, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink) const
	{
		Validate(InJson, InSink);
	}

	/** 认不认旧的比较字段（equals / gt …）：对象判断用 op / value，不认。 */
	virtual bool UsesCompareKeys() const { return true; }

	/** 判断此刻成不成立。在只读状态下调用（框架打开 FReadOnlyScope）。不等待、不稳住。 */
	virtual void Evaluate(const FJsonObject& InJson, FGameTestFlowContext& InContext, FGameTestCheckResult& OutResult) const = 0;

	/** 是不是反面判断（只在所在的屏在屏上时才算数）。 */
	virtual bool IsNegative(const FJsonObject& InJson) const { return false; }

	/** 给人看的一句（「界面 WBP_Shop 在屏上」），面板摘要、报告用。 */
	virtual FString Describe(const FJsonObject& InJson) const = 0;
};

/** 判断注册表。 */
class GAMETEST_API GameTestChecks
{
public:
	static TGameTestRegistry<IGameTestCheck>& Get();

	/** 第一个认这个判断对象的实现；都不认返回 nullptr。 */
	static const IGameTestCheck* FindFor(const FJsonObject& InJson);
};

/**
 * 比较（协议第 4.1 节「比较」）：`equals` / `ne` / `gt` / `gte` / `lt` / `lte` / `between` / `in` / `contains` / `matches`。
 * 判断、`invoke expect`、`api` 步骤的 `expect`、`number` 共用这一份，比较规则只写一次。
 */
namespace GameTestCompare
{
	/** 比较字段名。 */
	GAMETEST_API const TArray<FString>& Keys();

	/** 对象里写了比较。 */
	GAMETEST_API bool HasAny(const FJsonObject& InJson);

	/** 写法检查：值的类型（gt 要能转数字、matches 要是能编译的正则、between 是两个数）。 */
	GAMETEST_API void Validate(const FJsonObject& InJson, FGameTestDiagSink& InSink);

	/**
	 * 拿实际值比。期望值里可以写 `$var` 和 `=表达式`：`InResolve` 把 JSON 期望值变成 FGameTestValue
	 * （由条件树传进来，比较不依赖表达式模块）。没写比较时按真值算。
	 * 多个比较都要成立。`OutSay` 是不成立时的一句（「期望 >= 500，实际 120」）。
	 */
	GAMETEST_API bool Evaluate(const FJsonObject& InJson, const FGameTestValue& InActual, TFunctionRef<FGameTestValue(const TSharedPtr<class FJsonValue>&)> InResolve, FString& OutSay);

	/**
	 * 判断用的一步到位版本：期望值按 `InContext` 解析（`$var`、`=表达式`），结论、实际值、不成立的原因写进 `OutResult`；
	 * 实际值取不到（Unresolved）时算不成立并记下原因。各判断（value / api / expr / widget）共用。
	 */
	GAMETEST_API void EvaluateInto(const FJsonObject& InJson, const FGameTestValue& InActual, FGameTestFlowContext& InContext, FGameTestCheckResult& OutResult);

	/** 给人看的比较（「>= 500」「等于 "已领取"」），Describe 用。 */
	GAMETEST_API FString Describe(const FJsonObject& InJson);
}
