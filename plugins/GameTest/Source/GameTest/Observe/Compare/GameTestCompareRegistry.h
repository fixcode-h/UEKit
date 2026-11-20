// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Script/GameTestRegistry.h"
#include "Core/Script/GameTestValue.h"
#include "Observe/Ref/GameTestPropKind.h"

class FGameTestDiagSink;
class FGameTestFlowContext;
class FJsonValue;
struct FGameTestIdentItem;
struct FGameTestPropRead;

/** 一次比较的输入：实际值（带 kind）、判断项、已经求好的期望值和容差。 */
struct GAMETEST_API FGameTestCompareInput
{
	const FGameTestPropRead& Actual;
	const FGameTestIdentItem& Item;
	/** 期望值（`$var`、`=表达式` 已求值）；`between` 是两项数组，`in` 是数组；不要值的比较为 Null。 */
	FGameTestValue Expected;
	/** 文字比较忽略大小写（判断项 ignoreCase，默认运行配置 refDefaults.ignoreCase）。 */
	bool bIgnoreCase = true;
	/** eq / near 的容差（判断项 tol，默认按 kind 取 refDefaults.tol）。 */
	double Tol = 0.001;

	FGameTestCompareInput(const FGameTestPropRead& InActual, const FGameTestIdentItem& InItem)
		: Actual(InActual)
		, Item(InItem)
	{
	}
};

struct GAMETEST_API FGameTestCompareResult
{
	bool bHolds = false;
	/** 不成立时给人看的一句（「期望包含 购买成功，实际 金币不足」）。 */
	FString Why;
};

/**
 * 一种比较（docs/game-test-ref.md 第 3.4 节）：名字、适用的 kind、`Test`。
 * 面板 `shared/gt-script/ref/compare.ts` 有同名同表的一份（它的单测核对协议表格），名字和适用 kind 必须一致。
 * 一个分类一个文件（`Observe/Compare/GameTestCompare<分类>.cpp`），登记进 FGameTestCompareRegistry。
 */
class GAMETEST_API IGameTestCompare
{
public:
	virtual ~IGameTestCompare() = default;

	virtual FName GetKey() const = 0;

	/** 适用的 kind（协议表格「可用 op」那一列反过来）。 */
	virtual const TArray<EGameTestKind>& GetKinds() const = 0;

	/** 一句中文（面板比较下拉的说明）。 */
	virtual FString GetSummary() const = 0;

	/** 要不要 `value`（visible / hidden / isNull / empty… 不要）。 */
	virtual bool NeedsValue() const { return true; }

	/** 写法检查：`value` 的形状（between 两项、in 数组、matches 能编译）。 */
	virtual void ValidateValue(const TSharedPtr<FJsonValue>& InValue, FGameTestDiagSink& InSink) const {}

	virtual FGameTestCompareResult Test(const FGameTestCompareInput& InInput) const = 0;

	/** 给人看的比较词（「等于」「包含」）。 */
	virtual FString Verb() const = 0;

	/** 这个 kind 能不能用它（int / float 互收，visibility 收 enum 的）。 */
	bool Applies(EGameTestKind InKind) const;
};

/**
 * 用函数拼一个比较（各分类文件里一行登记一个，不必每个比较写一个类）。
 */
class GAMETEST_API FGameTestCompareFn final : public IGameTestCompare
{
public:
	using FTestFn = TFunction<FGameTestCompareResult(const FGameTestCompareInput&)>;
	using FValidateFn = TFunction<void(const TSharedPtr<FJsonValue>&, FGameTestDiagSink&)>;

	FGameTestCompareFn(FName InKey, const TArray<EGameTestKind>& InKinds, const FString& InVerb, const FString& InSummary, bool bInNeedsValue, FTestFn InTest, FValidateFn InValidate = nullptr);

	virtual FName GetKey() const override { return Key; }
	virtual const TArray<EGameTestKind>& GetKinds() const override { return Kinds; }
	virtual FString GetSummary() const override { return Summary; }
	virtual bool NeedsValue() const override { return bNeedsValue; }
	virtual void ValidateValue(const TSharedPtr<FJsonValue>& InValue, FGameTestDiagSink& InSink) const override;
	virtual FGameTestCompareResult Test(const FGameTestCompareInput& InInput) const override;
	virtual FString Verb() const override { return VerbText; }

private:
	FName Key;
	TArray<EGameTestKind> Kinds;
	FString VerbText;
	FString Summary;
	bool bNeedsValue = true;
	FTestFn TestFn;
	FValidateFn ValidateFn;
};

/** 比较注册表。 */
class GAMETEST_API FGameTestCompareRegistry
{
public:
	static TGameTestRegistry<IGameTestCompare>& Get();

	static const IGameTestCompare* Find(const FString& InOp);

	/** 这个 kind 能用的 op（登记顺序），`gt.props` 的 ops。 */
	static TArray<FString> OpsFor(EGameTestKind InKind);

	/** op 省略时按 kind 推：bool / visibility 的 `is`（期望 true），其余 `eq`。 */
	static FString DefaultOp(EGameTestKind InKind);

	/** 全部比较和适用 kind（`gt.actions` 的 compares，面板核对两边一致）。 */
	static TSharedRef<class FJsonObject> Describe();
};

/** 比较实现共用的小工具（各分类文件用）。 */
namespace GameTestCompareUtil
{
	/** 文字：字符串原样，别的转显示文字。 */
	GAMETEST_API FString TextOf(const FGameTestValue& InValue);

	/** 枚举成员名比较：`EX::A`、`A` 都认，也认整数（实际值是名字时按名字比）。 */
	GAMETEST_API bool EnumEquals(const FGameTestValue& InActual, const FGameTestValue& InExpected);

	/** 资源名或路径任一相等（`T_Icon`、`/Game/UI/T_Icon`、`/Game/UI/T_Icon.T_Icon`）。 */
	GAMETEST_API bool AssetEquals(const FString& InActual, const FString& InExpected);

	/** 向量类（vector / rotator / vector2 / color）拆成分量数组；拆不开返回 false。 */
	GAMETEST_API bool Components(const FGameTestValue& InValue, EGameTestKind InKind, TArray<double>& OutComponents);

	/** 逐分量差都不超过容差。 */
	GAMETEST_API bool ComponentsNear(const FGameTestValue& InActual, const FGameTestValue& InExpected, EGameTestKind InKind, double InTol);

	/** 两段文字相等（按 ignoreCase）。 */
	GAMETEST_API bool TextEquals(const FString& InA, const FString& InB, bool bInIgnoreCase);

	/** 一个值和期望值「相等」：按实际值的 kind（数字带容差、枚举认成员名、资源认名字或路径、文字按大小写开关）。 */
	GAMETEST_API bool ValueEquals(const FGameTestValue& InActual, const FGameTestValue& InExpected, EGameTestKind InKind, bool bInIgnoreCase, double InTol);

	/** 不成立的统一说法：「期望 等于 3，实际 5」。 */
	GAMETEST_API FGameTestCompareResult Result(bool bInHolds, const FGameTestCompareInput& InInput, const FString& InVerb);

	/** 写法检查：value 要是数组（in）、两项数字（between）、数字（count）、字符串。 */
	GAMETEST_API void ExpectArray(const TSharedPtr<FJsonValue>& InValue, int32 InExactCount, FGameTestDiagSink& InSink);
	GAMETEST_API void ExpectNumber(const TSharedPtr<FJsonValue>& InValue, FGameTestDiagSink& InSink);
	GAMETEST_API void ExpectString(const TSharedPtr<FJsonValue>& InValue, FGameTestDiagSink& InSink);
}
