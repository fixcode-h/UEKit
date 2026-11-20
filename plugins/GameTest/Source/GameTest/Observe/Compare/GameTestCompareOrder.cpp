// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Compare/GameTestCompareRegistry.h"

#include "Core/Script/GameTestDiag.h"
#include "Dom/JsonValue.h"
#include "Observe/Ref/GameTestPropPath.h"

namespace GameTestCompareOrderImpl
{
	using namespace GameTestCompareUtil;

	/** 两边都能转数字时按 InTest 比；比不了按不成立。 */
	FGameTestCompareResult Numbers(const FGameTestCompareInput& In, const TCHAR* InVerb, TFunctionRef<bool(double, double)> InTest)
	{
		double Actual = 0.0;
		double Expected = 0.0;
		if (!In.Actual.Value.TryNumber(Actual) || !In.Expected.TryNumber(Expected))
		{
			FGameTestCompareResult Out;
			Out.Why = FString::Printf(TEXT("%s 要两边都是数字（实际 %s，期望 %s）"), InVerb, *In.Actual.Value.ToDisplay(60), *In.Expected.ToDisplay(60));
			return Out;
		}
		return Result(InTest(Actual, Expected), In, InVerb);
	}

	FGameTestCompareResult Between(const FGameTestCompareInput& In)
	{
		const TArray<FGameTestValue>& Range = In.Expected.GetArray();
		double Actual = 0.0;
		double Low = 0.0;
		double High = 0.0;
		if (Range.Num() != 2 || !Range[0].TryNumber(Low) || !Range[1].TryNumber(High) || !In.Actual.Value.TryNumber(Actual))
		{
			FGameTestCompareResult Out;
			Out.Why = FString::Printf(TEXT("between 要 value 写 [下限, 上限] 两个数字，实际值是数字（实际 %s）"), *In.Actual.Value.ToDisplay(60));
			return Out;
		}
		return Result(Actual >= FMath::Min(Low, High) && Actual <= FMath::Max(Low, High), In, TEXT("在区间"));
	}
}

namespace GameTestBuiltin
{
	void RegisterCompareOrder()
	{
		using namespace GameTestCompareOrderImpl;
		using K = EGameTestKind;
		TGameTestRegistry<IGameTestCompare>& Registry = FGameTestCompareRegistry::Get();
		const TArray<K> Kinds = { K::Int, K::Float };
		const FGameTestCompareFn::FValidateFn Number = [](const TSharedPtr<FJsonValue>& V, FGameTestDiagSink& S) { GameTestCompareUtil::ExpectNumber(V, S); };
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("gt"), Kinds, TEXT("大于"), TEXT("大于"), true,
			[](const FGameTestCompareInput& In) { return Numbers(In, TEXT("大于"), [](double A, double B) { return A > B; }); }, Number));
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("gte"), Kinds, TEXT("不小于"), TEXT("大于等于"), true,
			[](const FGameTestCompareInput& In) { return Numbers(In, TEXT("不小于"), [](double A, double B) { return A >= B; }); }, Number));
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("lt"), Kinds, TEXT("小于"), TEXT("小于"), true,
			[](const FGameTestCompareInput& In) { return Numbers(In, TEXT("小于"), [](double A, double B) { return A < B; }); }, Number));
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("lte"), Kinds, TEXT("不大于"), TEXT("小于等于"), true,
			[](const FGameTestCompareInput& In) { return Numbers(In, TEXT("不大于"), [](double A, double B) { return A <= B; }); }, Number));
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("between"), Kinds, TEXT("在区间"), TEXT("在 [下限, 上限] 之间（含两端）"), true,
			&Between, [](const TSharedPtr<FJsonValue>& V, FGameTestDiagSink& S) { GameTestCompareUtil::ExpectArray(V, 2, S); }));
	}
}
