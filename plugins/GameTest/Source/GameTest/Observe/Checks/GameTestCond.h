// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Script/GameTestValue.h"

class FGameTestDiagSink;
class FGameTestFlowContext;
class FJsonObject;
class FJsonValue;
struct FGameTestExprScope;

/** 条件此刻的结论。 */
enum class EGameTestCondState : uint8
{
	Holds,
	NotHolds,
	/** 还不能判断：反面判断（`exists:false`…）所在的屏还没在屏上（协议第 4.2 节）。 */
	Unknown,
};

/** 一条判断的结果（报告 `payload.cond`、`gt.resolve conds` 的 parts）。 */
struct GAMETEST_API FGameTestCondPart
{
	/** 判断在条件树里的位置（`all.1.any.0`），根就是空。 */
	FString Path;
	EGameTestCondState State = EGameTestCondState::NotHolds;
	FGameTestValue Actual;
	FString Unresolved;
	FString Say;

	TSharedPtr<FJsonObject> ToJson() const;
};

struct GAMETEST_API FGameTestCondResult
{
	EGameTestCondState State = EGameTestCondState::NotHolds;
	TArray<FGameTestCondPart> Parts;

	bool Holds() const { return State == EGameTestCondState::Holds; }

	/** 给人看的一句：不成立的那几条的原因（成立时是实际值）。 */
	FString Summary() const;

	/** `{holds, unknown?, parts: […]}`。 */
	TSharedPtr<FJsonObject> ToJson() const;
};

/**
 * 统一条件（协议第 4 节）：`all` / `any` / `not` 任意嵌套，叶子是一条判断（IGameTestCheck）。
 * `if`、`loop`、`wait`、守护、`skipIf`、`assert`、`invoke expect` 都走这里。
 */
namespace GameTestCond
{
	/** 条件根上能写的、不属于判断的字段。 */
	GAMETEST_API const TArray<FString>& TreeKeys();

	/**
	 * 写法检查。`InExtraAllowed` 是条件之外宿主还允许的字段（skipIf 的 `also` / `verify`）。
	 * 1.6 的旧条件对象原样能过（它就是一条判断）。
	 */
	GAMETEST_API void Validate(const TSharedPtr<FJsonValue>& InCond, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink,
		const TArray<FString>& InExtraAllowed = TArray<FString>());

	/**
	 * 此刻成不成立（不等待、不稳住）。`InSubject` 是 `invoke expect` / `api expect` 的返回值：
	 * 判断里不写 `target` / `value` / `api` / `expr` 时比的就是它。在只读状态下判断。
	 */
	GAMETEST_API FGameTestCondResult Evaluate(const FJsonObject& InCond, FGameTestFlowContext& InContext,
		const FGameTestValue* InSubject = nullptr);

	/** 条件根上的 settleMs / elseSettleMs，没写用运行配置的默认值。 */
	GAMETEST_API int32 SettleMs(const FJsonObject& InCond, const FGameTestFlowContext& InContext);
	GAMETEST_API int32 ElseSettleMs(const FJsonObject& InCond, const FGameTestFlowContext& InContext);

	/** 给人看的一句（「界面 WBP_Shop 在屏上 且 金币 >= 500」）。 */
	GAMETEST_API FString Describe(const FJsonObject& InCond);

	/** 把一个 JSON 值读成条件对象（条件写成字符串时当表达式 `{"expr": …}`，方便 Agent 直接写 `"cond": "$n > 3"`）。 */
	GAMETEST_API TSharedPtr<FJsonObject> AsObject(const TSharedPtr<FJsonValue>& InCond);

	/** 比较的期望值求值（`$var`、`=表达式`、字面量）——判断实现里比较时传给 GameTestCompare。 */
	GAMETEST_API FGameTestValue ResolveExpected(const TSharedPtr<FJsonValue>& InValue, FGameTestFlowContext& InContext);

	/**
	 * Evaluate 期间 `InSubject` 放在块局部的这个名字下（不用全局变量）：判断实现（value）没写 `value` 时读它。
	 */
	GAMETEST_API const TCHAR* SubjectVar();
}

/**
 * 稳住（协议第 4.2 节）：连续成立 settleMs 才算成立，连续不成立 elseSettleMs 才算不成立。
 * `if` / `loop` / `wait` / 守护每帧调 `Tick`，拿到 Holds / NotHolds 才下结论。
 */
class GAMETEST_API FGameTestCondWatch
{
public:
	enum class EVerdict : uint8
	{
		Pending,
		Holds,
		NotHolds,
	};

	/** `bNeedNegativeSettle` = false 时不成立马上下结论（`wait until` 只关心成立）。 */
	void Start(const TSharedPtr<FJsonObject>& InCond, FGameTestFlowContext& InContext, bool bNeedNegativeSettle = true);

	EVerdict Tick(FGameTestFlowContext& InContext, double InNowSeconds);

	/** 最近一次判断（失败信息、报告、调试面板）。 */
	const FGameTestCondResult& GetLast() const { return Last; }

	const TSharedPtr<FJsonObject>& GetCond() const { return Cond; }

private:
	TSharedPtr<FJsonObject> Cond;
	FGameTestCondResult Last;
	double HoldSince = -1.0;
	double NotSince = -1.0;
	int32 Settle = 0;
	int32 ElseSettle = 0;
	bool bNegativeSettle = true;
};
