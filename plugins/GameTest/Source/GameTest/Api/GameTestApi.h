// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Script/GameTestRegistry.h"
#include "Core/Script/GameTestValue.h"

class FGameTestFlowContext;
class FJsonObject;
class FJsonValue;

/** 接口的一个参数（协议第 8 节）。 */
struct GAMETEST_API FGameTestApiArg
{
	FString Name;
	/** 类型（`obj`、`sel`、`cls`、`loc`、`string`、`number`、`int`、`bool`、`vector`、`cond`、`any`），面板据此给输入控件。 */
	FString Type = TEXT("any");
	bool bOptional = false;
	FString Note;
};

/** 接口的自描述：`gt.api`、面板接口选择器、表达式的只读检查都从这里来。 */
struct GAMETEST_API FGameTestApiDesc
{
	/** `Widget.IsShown`、`项目.QuestTest.AcceptQuest`。 */
	FString Name;
	/** 分类（`Widget`、`对象`……），面板分组用。 */
	FString Category;
	FString Summary;
	TArray<FGameTestApiArg> Args;
	/** 返回类型的写法（`bool`、`[obj]`、`{exists, visible}`），空表示不返回。 */
	FString Returns;
	/** 只读：能进条件和表达式。 */
	bool bReadOnly = false;
	/** 可能要跨帧（`Wait.*`、`Object.Find` 等到超时、`World.Open`）：条件和表达式里不能用，只能当步骤。 */
	bool bMayWait = false;
	/** `builtin` / `cpp` / `lua`。 */
	FString Src = TEXT("builtin");
	/** 显示名（项目接口的 DisplayName），空就用 Name。 */
	FString Display;
	/** 项目接口的源文件和行（`gt.api` 返回，面板点了跳过去）。 */
	FString File;
	int32 Line = 0;
	/** 提供它的模块（`GameTest`、`GameTestGAS`），可选模块卸载时按它摘。 */
	FString Module = TEXT("GameTest");

	TSharedPtr<FJsonObject> ToJson() const;
	/** `Widget.IsShown(sel) → bool`。 */
	FString Signature() const;
};

/** 一次调用的结果状态。 */
enum class EGameTestCallState : uint8
{
	Done,
	/** 还在等：`FGameTestApiCall::Poll` 每帧问一次，直到 Done / Failed。 */
	Pending,
	Failed,
};

/**
 * 一次调用：参数已经按 `FGameTestApiDesc::Args` 的顺序排好（缺了的可选参数是 Null），
 * 接口把结果写进 `Result`、出错写 `Error` + `ErrorCode`。
 */
struct GAMETEST_API FGameTestApiCall
{
	TArray<FGameTestValue> Args;
	FGameTestValue Result;
	FString Error;
	FName ErrorCode;
	/**
	 * 跨帧接口填这个：执行器每帧调一次（参数是当前时刻），返回 Done / Failed 时结束。
	 * 超时由步骤的 `timeoutMs` 管，接口自己不用算。
	 */
	TFunction<EGameTestCallState(double /*NowSeconds*/)> Poll;
	/** 跨帧接口中途被打断（超时、取消、守护插进来）时调一次，收拾半路状态。 */
	TFunction<void()> Cleanup;

	const FGameTestValue& Arg(int32 InIndex) const;
	/** 写错误并返回 Failed，接口里一行写完：`return Call.Fail(TEXT("arg_type"), TEXT("…"));` */
	EGameTestCallState Fail(FName InCode, const FString& InMessage);
	EGameTestCallState Done(const FGameTestValue& InResult);
};

/**
 * 接口（协议第 8 节）：自带接口一个分类一个文件（`Api/Builtin/GameTestApi<分类>.cpp`），
 * 项目接口（`_api.json`）也登记成它的实现。`api` 步骤、条件的 `api`、表达式里的调用都按名字找到它。
 */
class GAMETEST_API IGameTestApi
{
public:
	virtual ~IGameTestApi() = default;

	virtual const FGameTestApiDesc& GetDesc() const = 0;

	FName GetKey() const;

	/**
	 * 调用。`InContext.IsReadOnly()` 时只会调到 `bReadOnly` 的接口（框架先挡），实现里不用再查。
	 * 参数类型不对返回 Failed + `arg_type`，写清楚哪个参数要什么。
	 */
	virtual EGameTestCallState Call(FGameTestApiCall& InOutCall, FGameTestFlowContext& InContext) const = 0;
};

/**
 * 用一个函数实现的接口：自带接口大多十几行，一个分类的文件里连写几十个类太啰嗦，
 * 用它在登记函数里一个接口写一段。
 */
class GAMETEST_API FGameTestLambdaApi : public IGameTestApi
{
public:
	using FCallFn = TFunction<EGameTestCallState(FGameTestApiCall&, FGameTestFlowContext&)>;

	FGameTestLambdaApi(FGameTestApiDesc&& InDesc, FCallFn&& InFn);

	virtual const FGameTestApiDesc& GetDesc() const override { return Desc; }
	virtual EGameTestCallState Call(FGameTestApiCall& InOutCall, FGameTestFlowContext& InContext) const override;

private:
	FGameTestApiDesc Desc;
	FCallFn Fn;
};

/** 接口注册表。 */
class GAMETEST_API GameTestApis
{
public:
	static TGameTestRegistry<IGameTestApi>& Get();

	/**
	 * 一行登记一个函数实现的接口：
	 * `GameTestApis::Add(TEXT("Widget.IsShown"), TEXT("界面"), TEXT("界面在不在屏上"), {{TEXT("target"), TEXT("sel")}}, TEXT("bool"), true, Fn);`
	 */
	static void Add(const TCHAR* InName, const TCHAR* InCategory, const TCHAR* InSummary, TArray<FGameTestApiArg>&& InArgs,
		const TCHAR* InReturns, bool bReadOnly, FGameTestLambdaApi::FCallFn&& InFn, bool bMayWait = false);

	/** 改过名的项目接口（`_api.json` 的 `renamed`）：旧名 → 新名，调旧名时报「改用 …」（api_renamed）。 */
	static TMap<FString, FString>& GetRenamed();

	/** 卸掉某个模块（可选模块关闭）或某个来源（重读 `_api.json` 前清掉 cpp / lua）登记的全部接口。 */
	static void RemoveWhere(TFunctionRef<bool(const FGameTestApiDesc&)> InPredicate);

	/**
	 * 把参数排成 `Desc.Args` 的顺序：`InNamed`（`api` 步骤的 `args` 对象）或 `InPositional`（表达式里的调用）二选一。
	 * 缺了必填参数、多了不认识的参数名返回 false 和中文原因（arg_missing / unknown_field）。
	 */
	static bool BindArgs(const FGameTestApiDesc& InDesc, const TMap<FString, FGameTestValue>* InNamed,
		const TArray<FGameTestValue>* InPositional, TArray<FGameTestValue>& OutArgs, FName& OutCode, FString& OutError);

	/**
	 * 同步调用（条件、表达式）：找接口 → 只读检查（上下文在只读状态时）→ 不能跨帧检查 → 调。
	 * 结果或 Unresolved（带错误码：api_missing、api_renamed、expr_not_readonly……）。
	 */
	static FGameTestValue CallNow(const FString& InName, const TArray<FGameTestValue>& InPositional, FGameTestFlowContext& InContext);
};
