// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Script/GameTestRegistry.h"
#include "Core/Script/GameTestValue.h"

class FGameTestDiagSink;
class FGameTestFlowContext;

/**
 * 路径里的一段：`.成员`、`[下标]`、`.Fn(参数)`。
 *
 * 表达式解析器把 `@gi.Inventory.Items[0].GetCount()` 拆成来源 `gi` + 段列表，下标和参数已经求好值；
 * 来源和成员访问只看段，不再自己解析字符串——同一条路径写在条件、表达式、定位器 key 里结果一样。
 */
struct GAMETEST_API FGameTestPathSegment
{
	enum class EKind : uint8
	{
		/** `.Name` */
		Member,
		/** `[Index]`：Index 是数字（数组）或字符串（Map 键）。 */
		Index,
		/** `.Name(Args)` */
		Call,
	};

	EKind Kind = EKind::Member;
	FString Name;
	FGameTestValue Index;
	TArray<FGameTestValue> Args;

	/** 给人看的写法（`.Items`、`[0]`、`.GetCount()`），报「第几段取不到」时拼路径用。 */
	FString ToDisplay() const;

	static FGameTestPathSegment Member(const FString& InName);
	static FGameTestPathSegment At(const FGameTestValue& InIndex);
	static FGameTestPathSegment Call(const FString& InName, TArray<FGameTestValue>&& InArgs);
};

/**
 * 取值来源（协议第 4.3 节）：`@gi`、`@sub.<类名>`、`@actor[tag=X]`、`@luaMod.<路径>`、`@cvar.<名字>`……
 * 一种来源一个实现，一个文件（`Observe/Sources/GameTestSource<名字>.cpp`），登记进 `GameTestValueSources`。
 *
 * 来源只负责「根」：拿到根对象后，它可以自己吃掉开头几段（`@sub.UQuestSubsystem` 吃掉类名那段，
 * `@cvar.r.ScreenPercentage` 吃掉全部、`@luaMod.Game.Quest` 吃掉 require 路径），剩下的段交给通用的
 * 路径走法（`GameTestPath::Walk`：Map / 数组 / 向量直接取，UObject 交给成员访问器）。
 */
class GAMETEST_API IGameTestValueSource
{
public:
	virtual ~IGameTestValueSource() = default;

	/** 前缀，不带 `@`：`gi`、`sub`、`actor`、`luaMod`。 */
	virtual FName GetKey() const = 0;

	/** 一句中文说明（`gt.vars`、面板代码变量弹窗）。 */
	virtual FString GetSummary() const = 0;

	/** 写法示例：`@sub.<类名>.<属性>`。 */
	virtual FString GetSyntax() const = 0;

	/** 要不要 `[选择器]`（`@actor[tag=Boss]`）：要的话缺了是语法错。 */
	virtual bool UsesSelector() const { return false; }

	/** 根是不是 Lua（`@luaMod`、`@luaG`）：条件里调 Lua 函数受 `allowLuaInCond` 管。 */
	virtual bool IsLua() const { return false; }

	/**
	 * 取根。`InSelector` 是 `[...]` 里的原文（没有时为空），`InRest` 是后面的段；
	 * 来源吃掉了几段写进 `OutConsumed`（只取根就是 0）。取不到返回 Unresolved，原因写清楚是哪一段。
	 */
	virtual FGameTestValue Resolve(const FString& InSelector, TArrayView<const FGameTestPathSegment> InRest, int32& OutConsumed,
		FGameTestFlowContext& InContext) const = 0;

	/**
	 * 不开游戏时能查的错（选择器写法、类名在不在）。默认不查。
	 * 注意 InRest 里下标和参数可能还没求值（Null），只看名字。
	 */
	virtual void Validate(const FString& InSelector, TArrayView<const FGameTestPathSegment> InRest, FGameTestDiagSink& InSink) const
	{
	}
};

/**
 * 对象上的成员访问（读属性、调函数）：UObject 走反射、UnLua 实例走 Lua……一种访问方式一个实现。
 *
 * 为什么和来源分开：`@pc.Pawn.HealthComp.Health` 第二段以后每一段都是「在一个对象上取成员」，
 * 不管根从哪个来源来；把它写在某个来源里，别的来源就得再写一遍。
 * 多个访问器按 `GetPriority()` 从高到低问，第一个 `Handles` 的负责；它说没有这个成员时再问下一个
 * （同一个界面既有 C++ 属性又有 Lua 字段）。
 */
class GAMETEST_API IGameTestMemberAccess
{
public:
	virtual ~IGameTestMemberAccess() = default;

	virtual FName GetKey() const = 0;

	/** 大的先问。反射 0，Lua 实例 -10（同名时 C++ 属性优先，和 UnLua 的覆盖规则一致）。 */
	virtual int32 GetPriority() const { return 0; }

	/** 这个值归不归它管（一般看是不是 UObject、是不是某种类）。 */
	virtual bool Handles(const FGameTestValue& InBase) const = 0;

	/**
	 * 读成员。返回 false 表示「我这里没有这个成员」（交给下一个访问器）；
	 * 返回 true 时 `OutValue` 就是结果（可能是 Unresolved：成员在，但读的时候出错）。
	 */
	virtual bool ReadMember(const FGameTestValue& InBase, const FString& InName, FGameTestFlowContext& InContext,
		FGameTestValue& OutValue) const = 0;

	/**
	 * 调函数。规则同 ReadMember。`InContext.IsReadOnly()` 时只许调只读函数（BlueprintPure + const、
	 * 允许时的 Lua），不是只读的返回 true + Unresolved（code = expr_not_readonly）。
	 */
	virtual bool CallMethod(const FGameTestValue& InBase, const FString& InName, const TArray<FGameTestValue>& InArgs,
		FGameTestFlowContext& InContext, FGameTestValue& OutValue) const
	{
		return false;
	}
};

/** 来源注册表。主模块的来源在第一次用到时登记（GameTestBuiltins），可选模块在 StartupModule 里加。 */
class GAMETEST_API GameTestValueSources
{
public:
	static TGameTestRegistry<IGameTestValueSource>& Get();
};

/** 成员访问器注册表（按优先级排好）。 */
class GAMETEST_API GameTestMemberAccess
{
public:
	static void Register(const TSharedRef<IGameTestMemberAccess>& InAccess);
	static void Unregister(FName InKey);
	/** 按优先级从高到低。 */
	static const TArray<TSharedRef<IGameTestMemberAccess>>& GetAll();
};

/** 通用的路径走法。 */
namespace GameTestPath
{
	/**
	 * 从 `InBase` 开始一段段往下取：Map 按键、数组按下标（负数从尾数）、向量取 X/Y/Z、
	 * 字符串和数组的 `.Length` / `.Num`，其余交给成员访问器。
	 * 哪一段取不到就停下，返回 Unresolved（原因带上走到哪了：`@gi.Inventory 上没有 Items2`）。
	 * `InWhere` 是根的写法，只用来拼原因。
	 */
	GAMETEST_API FGameTestValue Walk(const FGameTestValue& InBase, TArrayView<const FGameTestPathSegment> InSegments,
		FGameTestFlowContext& InContext, const FString& InWhere);

	/** `@来源[选择器].段…` 整条取值：找来源 → Resolve → Walk 剩下的段。来源没登记时 Unresolved（path_missing）。 */
	GAMETEST_API FGameTestValue ResolveSource(FName InSource, const FString& InSelector,
		TArrayView<const FGameTestPathSegment> InSegments, FGameTestFlowContext& InContext);

	/** 在一个值上取一个成员（Walk 的单步，也给表达式的 `a.b` 用）。 */
	GAMETEST_API FGameTestValue Member(const FGameTestValue& InBase, const FString& InName, FGameTestFlowContext& InContext);

	/** 在一个值上调一个函数（表达式的 `a.Fn(x)`）。 */
	GAMETEST_API FGameTestValue CallMethod(const FGameTestValue& InBase, const FString& InName, const TArray<FGameTestValue>& InArgs,
		FGameTestFlowContext& InContext);

	/** 下标：数组按数字（负数从尾数），Map 按字符串键。 */
	GAMETEST_API FGameTestValue Index(const FGameTestValue& InBase, const FGameTestValue& InIndex);
}
