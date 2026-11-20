// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Observe/Ref/GameTestRef.h"

class FGameTestFlowContext;
class FJsonObject;
class FJsonValue;
class UObject;
class UWidget;

/** 一个候选：对象（控件 / Actor / 组件…），条目时带数据对象和位置。 */
struct GAMETEST_API FGameTestRefCandidate
{
	TWeakObjectPtr<UObject> Object;
	/** ListView 族条目的数据对象；还没显示出来的条目只有它，Object 为空。 */
	TWeakObjectPtr<UObject> Item;
	int32 Index = INDEX_NONE;
	/** 所在的容器控件（in 是 {obj, part} 时）：没显示的条目要它滚出来。 */
	TWeakObjectPtr<UWidget> Container;
};

/** 解析结果。 */
struct GAMETEST_API FGameTestRefResult
{
	/** 用到的引用定义（`$var` 时只有 Name）。 */
	FGameTestRef Ref;
	FGameTestRefCandidate Picked;
	/** only / ident / first / last / nearest / var。 */
	FString By;
	/** 按 in + class 收到几个（ident 过滤前）。 */
	int32 ClassCount = 0;
	/** ident 过滤后剩下的。 */
	TArray<FGameTestRefCandidate> Matches;
	/** 失败时的说明和错误码（ref_not_found / ref_ambiguous / ref_bad_class / ref_need_owner / ref_cycle / part_missing）。 */
	FString Error;
	FName Code;

	bool IsOk() const { return Picked.Object.IsValid(); }
	UObject* Get() const { return Picked.Object.Get(); }
	UObject* GetItem() const { return Picked.Item.Get(); }
};

/** 部件解析结果（第 2 节）。 */
struct GAMETEST_API FGameTestTargetResult
{
	FGameTestRefResult Ref;
	FGameTestTarget Target;
	/** part 找到的控件 / 组件；没写 part 时是对象本身。 */
	TWeakObjectPtr<UObject> Part;
	FString Error;
	FName Code;

	bool IsOk() const { return Part.IsValid(); }
	UObject* Get() const { return Part.Get(); }
	UWidget* GetWidget() const;
};

/**
 * 对象引用的解析（docs/game-test-ref.md 第 1 节）：按 `in` + `class` 收候选 → 按 `ident` 过滤 → 按 `many` 挑。
 * 0 个时不等：调用方按步骤的 timeoutMs 每帧再调（条件每帧求值、动作的目标解析本来就重试）。
 * 只在游戏线程上用；只读（没显示的 ListView 条目在非只读时会顺手滚出来，见 bAllowScroll）。
 */
namespace GameTestRefResolve
{
	/** 引用的三种写法之一（命名 / `$var` / 内联）。 */
	GAMETEST_API FGameTestRefResult Resolve(const TSharedPtr<FJsonValue>& InRef, FGameTestFlowContext& InContext);

	GAMETEST_API FGameTestRefResult ResolveRef(const FGameTestRef& InRef, FGameTestFlowContext& InContext);

	/** 部件写法（或直接一个引用）。 */
	GAMETEST_API FGameTestTargetResult ResolveTarget(const TSharedPtr<FJsonValue>& InTarget, FGameTestFlowContext& InContext);

	/** 名字或 `$var` → 引用定义（命名引用从 `objects` 取）。内联直接读。`$var` 时 OutDirect 是变量里的对象。 */
	GAMETEST_API bool ToRef(const TSharedPtr<FJsonValue>& InRef, FGameTestFlowContext& InContext, FGameTestRef& OutRef, UObject*& OutDirect, FString& OutError);

	/** 只按 in + class 收（gt.props 算「同类里」那一列、录制判断同类有几个）。 */
	GAMETEST_API bool Collect(const FGameTestRef& InRef, FGameTestFlowContext& InContext, TArray<FGameTestRefCandidate>& OutCandidates, FString& OutError, FName& OutCode);

	/** 候选满不满足 ident（全部满足）。`OutActuals` 是每条的实际值（报告、歧义说明）。 */
	GAMETEST_API bool MatchesIdent(const FGameTestRefCandidate& InCandidate, const TArray<FGameTestIdentItem>& InIdent, FGameTestFlowContext& InContext, TArray<FString>* OutActuals = nullptr);

	/** 报告的 `payload.ref`（写进 InContext.StepPayload，没有时不写）。 */
	GAMETEST_API void WritePayload(const FGameTestRefResult& InResult, FGameTestFlowContext& InContext);

	/** 给人看的候选（显示名 + ident 的实际值），歧义报错、gt.resolve 用。 */
	GAMETEST_API TSharedPtr<FJsonObject> CandidateToJson(const FGameTestRefCandidate& InCandidate, const TArray<FGameTestIdentItem>& InIdent, FGameTestFlowContext& InContext);

	/** 对象的显示名（类名 + 文字 / 标签）。 */
	GAMETEST_API FString DisplayName(const UObject* InObject);
}
