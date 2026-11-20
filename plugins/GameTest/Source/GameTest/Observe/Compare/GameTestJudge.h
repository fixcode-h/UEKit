// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Observe/Ref/GameTestPropPath.h"

class FGameTestFlowContext;
struct FGameTestIdentItem;
struct FGameTestScriptOptions;

/** 一条判断项对着一个对象的结果。 */
struct GAMETEST_API FGameTestJudgeResult
{
	bool bHolds = false;
	/** 属性读不到、期望值算不出：按不成立算，但报告写原因。 */
	bool bUnresolved = false;
	FGameTestPropRead Read;
	/** 实际用的比较（省略时按 kind 推出来的）。 */
	FString Op;
	/** 不成立时的一句（读不到哪一段 / 期望和实际）。 */
	FString Why;
	/** op_type / op_unknown / prop_missing…：写法问题，报告和 gt.resolve 标出来。 */
	FName Code;
};

/**
 * 判断项求值（docs/game-test-ref.md 第 3.3 节）：读属性 → 求期望值（`$var`、`=表达式`）→ 按 kind 选比较 → 比。
 * 引用的 ident 过滤、条件里的对象判断、obj.wait、gt.resolve 都走这里。只读，回放时每帧调。
 */
namespace GameTestJudge
{
	GAMETEST_API FGameTestJudgeResult Test(UObject* InObject, const FGameTestIdentItem& InItem, FGameTestFlowContext& InContext, UObject* InListItem = nullptr);

	/** 已经读好的值（gt.props 的 matches、录制核对）。 */
	GAMETEST_API FGameTestJudgeResult TestRead(const FGameTestPropRead& InRead, const FGameTestIdentItem& InItem, FGameTestFlowContext& InContext);

	/** eq / near 的默认容差（运行配置 refDefaults.tol）。 */
	GAMETEST_API double DefaultTol(EGameTestKind InKind, const FGameTestScriptOptions& InOptions);

	/** op 和 kind 对不上时的说明（空 = 对得上）：深度检查报 op_type 用。 */
	GAMETEST_API FString TypeMismatch(const FString& InOp, EGameTestKind InKind);
}
