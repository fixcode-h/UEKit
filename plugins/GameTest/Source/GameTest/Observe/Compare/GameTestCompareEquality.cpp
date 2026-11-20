// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Compare/GameTestCompareRegistry.h"

#include "Observe/Ref/GameTestPropPath.h"

namespace GameTestCompareEqualityImpl
{
	using namespace GameTestCompareUtil;

	/** is：bool 按真值（value 省略 = true）；枚举认成员名或整数。 */
	FGameTestCompareResult Is(const FGameTestCompareInput& In)
	{
		const EGameTestKind Kind = In.Actual.Kind;
		if (Kind == EGameTestKind::Bool)
		{
			const bool bExpected = In.Expected.IsNull() ? true : In.Expected.Truthy();
			return Result(In.Actual.Value.Truthy() == bExpected, In, TEXT("是"));
		}
		return Result(EnumEquals(In.Actual.Value, In.Expected.IsNull() ? FGameTestValue::FromBool(true) : In.Expected), In, TEXT("是"));
	}

	FGameTestCompareResult IsNot(const FGameTestCompareInput& In)
	{
		return Result(!EnumEquals(In.Actual.Value, In.Expected), In, TEXT("不是"));
	}

	FGameTestCompareResult Eq(const FGameTestCompareInput& In)
	{
		return Result(ValueEquals(In.Actual.Value, In.Expected, In.Actual.Kind, In.bIgnoreCase, In.Tol), In, TEXT("等于"));
	}

	FGameTestCompareResult Ne(const FGameTestCompareInput& In)
	{
		return Result(!ValueEquals(In.Actual.Value, In.Expected, In.Actual.Kind, In.bIgnoreCase, In.Tol), In, TEXT("不等于"));
	}
}

namespace GameTestBuiltin
{
	void RegisterCompareEquality()
	{
		using namespace GameTestCompareEqualityImpl;
		using K = EGameTestKind;
		TGameTestRegistry<IGameTestCompare>& Registry = FGameTestCompareRegistry::Get();
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("is"), TArray<K>{ K::Bool, K::Enum, K::Visibility }, TEXT("是"),
			TEXT("bool 按真值（value 省略 = true）；枚举写成员名（EX::A 或 A）或整数"), false, &Is));
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("isNot"), TArray<K>{ K::Enum, K::Visibility }, TEXT("不是"),
			TEXT("枚举不是这个成员"), true, &IsNot));
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("eq"), TArray<K>{ K::Int, K::Float, K::String, K::Text, K::Asset, K::Vector, K::Rotator, K::Vector2 }, TEXT("等于"),
			TEXT("相等：数字带容差 tol，文字按 ignoreCase，资源认名字或路径，向量逐分量带 tol"), true, &Eq));
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("ne"), TArray<K>{ K::Int, K::Float, K::String, K::Text }, TEXT("不等于"),
			TEXT("不相等"), true, &Ne));
	}
}
