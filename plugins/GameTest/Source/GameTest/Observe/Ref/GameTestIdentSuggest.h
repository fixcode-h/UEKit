// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Observe/Ref/GameTestPropPath.h"
#include "Observe/Ref/GameTestRef.h"

class FGameTestFlowContext;
class FJsonObject;
class FJsonValue;
struct FGameTestRefCandidate;

/** 一个能当标识的属性（gt.props 的一项、录制挑标识的候选）。 */
struct GAMETEST_API FGameTestIdentProp
{
	/** 能直接写进脚本的路径（`@self.ShopId`、`@TxtTitle:text`）。 */
	FString Prop;
	FString Name;
	/** self / item / lua / itemLua / part:<名字>：gt.props 按它分组。 */
	FString Group;
	FGameTestPropRead Read;
	/** 原始分（GameTestItemKey::Score 的口径）；gt.props 换成 0–10。 */
	double Score = 0.0;
	FString Why;
};

/**
 * 挑标识（docs/game-test-ref.md 第 7.3 节 suggest、第 8 节录制）：列出对象可以当标识的属性，
 * 再从同类候选里挑最少的能把目标区分出来的组合（按 identRules 先、再按分数）。
 */
namespace GameTestIdentSuggest
{
	/** 对象可以当标识的属性：自己声明的字段（不含 UMG / 引擎基类）、Lua 字段、条目数据对象的字段、文字部件的 :text / :textKey。 */
	GAMETEST_API void Collect(UObject* InObject, UObject* InItem, FGameTestFlowContext& InContext, TArray<FGameTestIdentProp>& OutProps);

	/**
	 * 在 InSiblings（同 in 同类的全部候选，含目标自己）里把 InTarget 区分出来的最少组合（最多 InMax 条）。
	 * 同类只有它一个时 OutIdent 为空、返回 true；区分不开返回 false（OutIdent 是分最高的那几条）。
	 */
	GAMETEST_API bool Pick(const FGameTestRefCandidate& InTarget, const TArray<FGameTestRefCandidate>& InSiblings, FGameTestFlowContext& InContext, TArray<FGameTestIdentItem>& OutIdent, int32 InMax = 3);

	/** 运行配置里给这个类的规则（identRules / closeRules，按继承最近的优先）；没有返回 false。 */
	GAMETEST_API bool RuleFor(const UClass* InClass, const TSharedPtr<FJsonValue>& InRules, TSharedPtr<FJsonObject>& OutRule);

	/** 按 identRules 给目标写 ident（规则里的 prop 读目标此刻的值）；没有规则返回 false。 */
	GAMETEST_API bool FromRule(const FGameTestRefCandidate& InTarget, FGameTestFlowContext& InContext, TArray<FGameTestIdentItem>& OutIdent);

	/** 读到的值 → 判断项（op 按 kind：bool 是 is，其余 eq）。 */
	GAMETEST_API FGameTestIdentItem ItemFor(const FString& InProp, const FGameTestPropRead& InRead);

	/** 原始分 → 0–10。 */
	GAMETEST_API int32 Score10(double InScore);
}
