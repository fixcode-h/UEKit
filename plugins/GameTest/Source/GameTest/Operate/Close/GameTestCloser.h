// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Script/GameTestRegistry.h"

class FGameTestDiagSink;
class FGameTestFlowContext;
class FJsonObject;
class FJsonValue;
class UObject;
struct FGameTestStepContext;

/** 一次关闭的环境：对象、这一条 how、结果说明。 */
struct GAMETEST_API FGameTestCloseRun
{
	FGameTestStepContext& Step;
	FGameTestFlowContext& Flow;
	/** 引用解析到的对象（不是 part：part 由各关闭方式按 how 里的 part 自己找）。 */
	UObject* Object = nullptr;
	const FJsonObject& How;
	/** 做了什么 / 为什么做不了（写进 payload.close 和失败消息）。 */
	FString Message;

	FGameTestCloseRun(FGameTestStepContext& InStep, FGameTestFlowContext& InFlow, UObject* InObject, const FJsonObject& InHow)
		: Step(InStep), Flow(InFlow), Object(InObject), How(InHow)
	{
	}

	/** how 里的 part（省略 = 对象本身）；找不到返回空并写 Message。 */
	UObject* PartOrSelf();
};

/**
 * 一种关闭方式（docs/game-test-ref.md 5.2 的 `by`）：call / lua / click / visibility / remove / pop / key / destroy。
 * 一种一个文件（`Operate/Close/GameTestClose<名字>.cpp`；`pop` 在 GameTestCommonUI 模块，没装时报 close_unavailable）。
 *
 * `Close` 在这一帧里做完（调函数、点按钮、发按键都是同步的）；「关没关掉」由 obj.close 按 verify 跨帧核对，
 * 没过就试下一种，所以实现不用自己等。
 */
class GAMETEST_API IGameTestCloser
{
public:
	virtual ~IGameTestCloser() = default;

	/** `by` 的值。 */
	virtual FName GetKey() const = 0;

	virtual FString GetSummary() const = 0;

	/** 除 `by` 外认的字段（加载时拼错的字段报出来）。 */
	virtual const TArray<FString>& GetFields() const = 0;

	/** 加载时查字段的值（必填、类型）。 */
	virtual void Validate(const FJsonObject& InHow, FGameTestDiagSink& InSink) const {}

	/** 做一次。做不了（对象类型不对、函数不存在、设置不允许）返回 false，原因写进 InRun.Message。 */
	virtual bool Close(FGameTestCloseRun& InRun) const = 0;

	/**
	 * `how` 省略、也没有 closeRules 时的推荐（5.2）：这个对象能用这种方式关时给出一条 how（如带函数名的 call），否则空。
	 * 推荐顺序看 GetRecommendOrder（小的先试）；不参与推荐的返回 INDEX_NONE。
	 */
	virtual TSharedPtr<FJsonObject> Recommend(UObject* InObject) const { return nullptr; }
	virtual int32 GetRecommendOrder() const { return INDEX_NONE; }
};

namespace GameTestClosers
{
	GAMETEST_API TGameTestRegistry<IGameTestCloser>& Get();

	/** 一条 how 的写法（by 认不认得、字段对不对）。 */
	GAMETEST_API void ValidateHow(const TSharedPtr<FJsonValue>& InHow, FGameTestDiagSink& InSink);

	/**
	 * `how` 省略时用什么：运行配置 closeRules 里这个类（按继承，最近的优先）的 how，没有按推荐。
	 * `OutVerify` 是规则里写的 verify（没写为空）；`OutFrom` 是 `closeRules` / `recommend`。
	 */
	GAMETEST_API TArray<TSharedPtr<FJsonObject>> DefaultHows(UObject* InObject, FGameTestFlowContext& InFlow, FString& OutVerify, FString& OutFrom);
}
