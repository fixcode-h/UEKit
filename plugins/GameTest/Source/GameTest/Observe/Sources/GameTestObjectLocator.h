// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Observe/Ref/GameTestRef.h"

class FJsonObject;
class FJsonValue;
class UObject;
class UWorld;
class FGameTestFlowContext;
struct FGameTestValue;

/**
 * 接口库（Object.*、gt.vars 的对象作用域）用的对象定位：2.0.0 起就是对象引用（docs/game-test-ref.md 第 1 节），
 * 这里只是给接口库留的薄包装，解析全走 GameTestRefResolve，不另写一套。
 *
 * 参数写类名字符串（= `{class}`）或引用对象 `{class, in?, ident?, many?, near?, client?}`。
 */
struct GAMETEST_API FGameTestObjectLocator
{
	/** = Ref.Class（调用方常用，单独留一份）。 */
	FString Class;
	FGameTestRef Ref;

	/** 解析并校验（不认的字段、1.x 的 key / component 都报出来）。 */
	static bool Parse(const FJsonObject& InJson, FGameTestObjectLocator& OutLocator, TArray<FString>& OutErrors);

	TSharedRef<FJsonObject> ToJson() const;

	FString Describe() const;
};

enum class EGameTestLocate : uint8
{
	Found,
	/** 0 个：调用方等到超时。 */
	NotFound,
	/** 命中多个且 many = error：报 ref_ambiguous。 */
	Ambiguous,
	/** 引用本身有问题（组件没写 in、来源路径错）：等也没用，直接失败。 */
	Error,
};

struct GAMETEST_API FGameTestLocateResult
{
	UObject* Picked = nullptr;
	/** 按什么挑的：`only` / `ident` / `first` / `last` / `nearest`。 */
	FString By;
	int32 Candidates = 0;
	/** 失败时的说明（候选和它们的标识值）。 */
	FString Message;

	/** 报告的 `payload.ref`。 */
	TSharedRef<FJsonObject> ToJson(const FString& InName, const FGameTestObjectLocator& InLocator) const;
};

class GAMETEST_API GameTestObjectLocate
{
public:
	/** 按引用找一次（不等待）。 */
	static EGameTestLocate Locate(const FGameTestObjectLocator& InLocator, FGameTestFlowContext& InContext, FGameTestLocateResult& OutResult);

	/** 全部候选（Object.FindAll、Object.Count 用）：已按 ident 过滤。 */
	static bool FindAll(const FGameTestObjectLocator& InLocator, FGameTestFlowContext& InContext, TArray<UObject*>& OutObjects, FString& OutError);

	/**
	 * `objects` 里的名字（`{"$obj": "名字"}`、`@obj.名字`）→ 对象。找不到 Unresolved（ref_not_found，调用方按步骤超时等）、
	 * 多个 ref_ambiguous；找到时往 `StepPayload` 写 `payload.ref`。
	 */
	static FGameTestValue ResolveNamed(const FString& InName, FGameTestFlowContext& InContext);

	/** 读对象上一条属性路径（属性路径 v5：`@.X`、`@#X`、`@:text`；不带 @ 的按 `@.` 读，`#X` 按 `@#X` 读）。 */
	static FGameTestValue ReadKey(UObject* InObject, const FString& InPath, FGameTestFlowContext& InContext);
};
