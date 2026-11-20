// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Compare/GameTestCompareRegistry.h"

#include "Observe/Ref/GameTestPropPath.h"

namespace GameTestCompareMembershipImpl
{
	/** in：等于数组里任一项（相等规则同 eq / is）。 */
	FGameTestCompareResult In(const FGameTestCompareInput& InInput)
	{
		for (const FGameTestValue& Candidate : InInput.Expected.GetArray())
		{
			if (GameTestCompareUtil::ValueEquals(InInput.Actual.Value, Candidate, InInput.Actual.Kind, InInput.bIgnoreCase, InInput.Tol))
			{
				return GameTestCompareUtil::Result(true, InInput, TEXT("是其中之一"));
			}
		}
		return GameTestCompareUtil::Result(false, InInput, TEXT("是其中之一"));
	}
}

namespace GameTestBuiltin
{
	void RegisterCompareMembership()
	{
		using K = EGameTestKind;
		FGameTestCompareRegistry::Get().Register(MakeShared<FGameTestCompareFn>(TEXT("in"), TArray<K>{ K::Int, K::Float, K::String, K::Text, K::Enum, K::Visibility }, TEXT("是其中之一"),
			TEXT("等于数组里任一项"), true, &GameTestCompareMembershipImpl::In,
			[](const TSharedPtr<FJsonValue>& V, FGameTestDiagSink& S) { GameTestCompareUtil::ExpectArray(V, 0, S); }));
	}
}
