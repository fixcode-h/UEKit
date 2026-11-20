// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Compare/GameTestCompareRegistry.h"

#include "Observe/Ref/GameTestPropPath.h"
#include "Observe/Ref/GameTestRef.h"

namespace GameTestCompareArrayImpl
{
	using namespace GameTestCompareUtil;

	int32 CountOf(const FGameTestValue& InValue)
	{
		return FMath::Max(0, InValue.Len());
	}

	/** count：项数按 cmp（默认 eq）和 value 比。 */
	FGameTestCompareResult Count(const FGameTestCompareInput& In)
	{
		const int32 Num = CountOf(In.Actual.Value);
		double Expected = 0.0;
		FGameTestCompareResult Out;
		if (!In.Expected.TryNumber(Expected))
		{
			Out.Why = TEXT("count 的 value 要写数字");
			return Out;
		}
		const FString Cmp = In.Item.Cmp.IsEmpty() ? FString(TEXT("eq")) : In.Item.Cmp;
		const double Actual = static_cast<double>(Num);
		Out.bHolds = Cmp == TEXT("ne") ? Actual != Expected
			: Cmp == TEXT("gt") ? Actual > Expected
			: Cmp == TEXT("gte") ? Actual >= Expected
			: Cmp == TEXT("lt") ? Actual < Expected
			: Cmp == TEXT("lte") ? Actual <= Expected
			: FMath::IsNearlyEqual(Actual, Expected);
		if (!Out.bHolds)
		{
			Out.Why = FString::Printf(TEXT("期望项数 %s %g，实际 %d 项"), *Cmp, Expected, Num);
		}
		return Out;
	}

	/** has：数组里有一项等于 value；Map 时看键。 */
	FGameTestCompareResult Has(const FGameTestCompareInput& In)
	{
		const FGameTestValue& Actual = In.Actual.Value;
		if (Actual.Type == EGameTestValueType::Map)
		{
			FGameTestValue Unused;
			return Result(Actual.TryGetField(TextOf(In.Expected), Unused), In, TEXT("含有"));
		}
		for (const FGameTestValue& Item : Actual.GetArray())
		{
			if (Item.LooseEquals(In.Expected))
			{
				return Result(true, In, TEXT("含有"));
			}
		}
		return Result(false, In, TEXT("含有"));
	}

	FGameTestCompareResult Empty(const FGameTestCompareInput& In)
	{
		return Result(CountOf(In.Actual.Value) == 0, In, TEXT("为空"));
	}

	FGameTestCompareResult NotEmpty(const FGameTestCompareInput& In)
	{
		return Result(CountOf(In.Actual.Value) > 0, In, TEXT("不为空"));
	}
}

namespace GameTestBuiltin
{
	void RegisterCompareArray()
	{
		using namespace GameTestCompareArrayImpl;
		using K = EGameTestKind;
		TGameTestRegistry<IGameTestCompare>& Registry = FGameTestCompareRegistry::Get();
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("count"), TArray<K>{ K::Array }, TEXT("项数"), TEXT("项数按 cmp（eq / ne / gt / gte / lt / lte，默认 eq）和 value 比"), true, &Count,
			[](const TSharedPtr<FJsonValue>& V, FGameTestDiagSink& S) { GameTestCompareUtil::ExpectNumber(V, S); }));
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("has"), TArray<K>{ K::Array }, TEXT("含有"), TEXT("含有等于 value 的一项（Map 看键）"), true, &Has));
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("empty"), TArray<K>{ K::Array }, TEXT("为空"), TEXT("一项都没有"), false, &Empty));
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("notEmpty"), TArray<K>{ K::Array }, TEXT("不为空"), TEXT("至少一项"), false, &NotEmpty));
	}
}
