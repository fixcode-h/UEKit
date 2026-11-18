// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Script/GameTestRegistry.h"
#include "Core/Script/GameTestValue.h"

class FGameTestFlowContext;

/**
 * 表达式的内置函数（协议第 3 节）：`len`、`round`、`text(sel)`、`vec`……
 * 一个分类一个文件（`Core/Expr/Functions/GameTestExpr<分类>.cpp`），登记进 `GameTestExprFunctions`。
 *
 * 内置函数都只读（表达式只算值、不改游戏）。表达式里写的名字先找这里，找不到再找只读接口
 * （`Widget.IsShown(...)`）——所以内置函数名不带点，接口名都带分类点，不会撞。
 */
struct GAMETEST_API FGameTestExprFunction
{
	using FCallFn = TFunction<FGameTestValue(const TArray<FGameTestValue>& /*Args*/, FGameTestFlowContext& /*Context*/)>;

	FName Name;
	/** 参数个数范围；MaxArgs < 0 表示不限（`min(…)`、`format(fmt, …)`）。解析时就查个数。 */
	int32 MinArgs = 0;
	int32 MaxArgs = 0;
	/** 分类（`数值`、`文字`、`控件`），面板表达式框的补全分组。 */
	FString Category;
	/** `round(x, n?)`。 */
	FString Signature;
	FString Summary;
	/**
	 * 参数里有 Unresolved 时也调（`exists(sel)`：控件不在本身就是答案）。默认 false：
	 * 任一参数取不到，结果直接是那个 Unresolved，函数实现不用每个都判。
	 */
	bool bAcceptsUnresolved = false;
	FCallFn Call;

	FName GetKey() const { return Name; }
};

/** 表达式函数注册表。 */
class GAMETEST_API GameTestExprFunctions
{
public:
	static TGameTestRegistry<FGameTestExprFunction>& Get();

	/** 一行登记：`GameTestExprFunctions::Add(TEXT("len"), 1, 1, TEXT("文字"), TEXT("len(x)"), TEXT("长度"), Fn);` */
	static void Add(const TCHAR* InName, int32 InMinArgs, int32 InMaxArgs, const TCHAR* InCategory, const TCHAR* InSignature, const TCHAR* InSummary, FGameTestExprFunction::FCallFn&& InFn, bool bAcceptsUnresolved = false);
};
