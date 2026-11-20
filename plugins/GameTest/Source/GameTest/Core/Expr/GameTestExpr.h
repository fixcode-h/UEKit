// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Script/GameTestValue.h"

class FGameTestDiagSink;
class FGameTestFlowContext;
struct FGameTestExprNode;

/** 表达式静态检查时知道的东西（`gt.validate`、加载场景）。 */
struct GAMETEST_API FGameTestExprScope
{
	/** 这里能用的 `$名字`（params、vars、外层循环的 as / index、之前 set / saveAs 过的）；为空表示不查变量。 */
	const TSet<FString>* KnownVars = nullptr;
	/** 条件里：只许只读（默认）。 */
	bool bReadOnly = true;
	/** `_settings.json` 的 allowLuaInCond。 */
	bool bAllowLuaInCond = false;
	/** 场景 `objects` 段里的名字（对象引用写名字时查在不在）；为空表示不查。 */
	const TSet<FString>* KnownObjects = nullptr;
};

/**
 * 编译好的表达式（协议第 3 节）。只读：只算值、不改游戏。
 *
 * 一份文字解析一次（`GameTestExprCache`），回放时每帧求值只走语法树。
 * 语法错在解析时给出位置（第几个字），类型和函数在 `Validate` 里查，运行时取不到的返回 Unresolved。
 */
class GAMETEST_API FGameTestExpr
{
public:
	/** 长度上限（协议第 3 节）。 */
	static constexpr int32 MaxLength = 1000;

	/**
	 * 解析。`InText` 不带开头的 `=`。失败返回空，`OutError` 是「第 12 个字附近：缺右括号」，`OutCode` 是 expr_syntax。
	 */
	static TSharedPtr<const FGameTestExpr> Parse(const FString& InText, FString& OutError);

	/** 求值：在只读状态下算（打开 FReadOnlyScope），结果可能是 Unresolved。 */
	FGameTestValue Evaluate(FGameTestFlowContext& InContext) const;

	/** 静态检查：函数在不在、参数个数、接口是不是只读、`$变量` 有没有定义、Lua 调用许不许。 */
	void Validate(const FGameTestExprScope& InScope, FGameTestDiagSink& InSink) const;

	/** 用到的 `$名字`（报告 `payload.vars` 只记用到的变量）。 */
	void CollectVars(TSet<FString>& OutVars) const;

	const FString& GetText() const { return Text; }

	FGameTestExpr(const FString& InText, TSharedRef<FGameTestExprNode> InRoot);
	~FGameTestExpr();

private:
	FString Text;
	TSharedRef<FGameTestExprNode> Root;
};

/** 解析缓存：同一段文字只解析一次。只在游戏线程上用。 */
class GAMETEST_API GameTestExprCache
{
public:
	/** 取编译好的表达式；语法错时返回空、`OutError` 是原因（同一段错的文字也缓存原因，不反复解析）。 */
	static TSharedPtr<const FGameTestExpr> Get(const FString& InText, FString& OutError);

	/** 求一段表达式文字的值（解析 + 求值），语法错返回 Unresolved（expr_syntax）。 */
	static FGameTestValue Eval(const FString& InText, FGameTestFlowContext& InContext);

	/** 换场景、重读 _api.json 时清掉（只是为了不无限长大）。 */
	static void Reset();
};
