// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Script/GameTestValue.h"

/**
 * 表达式语法树（只在 Core/Expr 里用）。
 *
 * 路径单独成一种节点（根 + 一串取成员 / 下标 / 调用）而不是一层层嵌套：
 * 取值来源要能「吃掉开头几段」（`@sub.UQuestSubsystem`、`@luaMod.Game.Quest`），得一次看到整条路径。
 */
enum class EGameTestExprKind : uint8
{
	Literal,
	/** 根 + 后缀（`$var.a[0]`、`@gi.X`、`(expr).Y`）。 */
	Path,
	/** 内置函数或接口：`len(x)`、`Widget.IsShown("WBP_Shop")`。 */
	Call,
	Unary,
	Binary,
	Ternary,
	ArrayLit,
	MapLit,
};

enum class EGameTestPathRoot : uint8
{
	/** `$名字` */
	Var,
	/** `@来源[选择器]` */
	Source,
	/** 括号里的子表达式 / 函数调用结果 / 字面量后面接成员 */
	Expr,
};

struct FGameTestExprNode;

/** 路径的一段后缀。 */
struct FGameTestExprPostfix
{
	enum class EKind : uint8
	{
		Member,
		Index,
		Call,
	};
	EKind Kind = EKind::Member;
	FString Name;
	/** Index：下标表达式；Call：参数。 */
	TArray<TSharedRef<FGameTestExprNode>> Args;
};

struct FGameTestExprNode
{
	EGameTestExprKind Kind = EGameTestExprKind::Literal;
	/** 在原文里的位置（第几个字，从 0），报错用。 */
	int32 Pos = 0;

	//~ Literal
	FGameTestValue Value;

	//~ Path
	EGameTestPathRoot Root = EGameTestPathRoot::Var;
	/** Var：变量名；Source：来源前缀（不带 @）。 */
	FString RootName;
	/** Source 的 `[...]` 原文。 */
	FString Selector;
	TArray<FGameTestExprPostfix> Postfix;

	//~ Call：函数名（可以带点：`Widget.IsShown`、`项目.QuestTest.Get`）
	FString Function;

	//~ Unary / Binary：运算符（`-`、`not`、`+`、`==`、`in`、`and`…）
	FString Op;

	//~ 子节点：Unary 1 个，Binary 2 个，Ternary 3 个，Call 的参数，ArrayLit 的元素，MapLit 的值；
	//~ Path 根是 Expr 时第 0 个是根表达式。
	TArray<TSharedRef<FGameTestExprNode>> Children;
	/** MapLit 的键（和 Children 一一对应）。 */
	TArray<FString> Keys;
};
