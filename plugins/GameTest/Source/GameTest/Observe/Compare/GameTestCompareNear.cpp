// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Compare/GameTestCompareRegistry.h"

#include "Observe/Ref/GameTestPropPath.h"

namespace GameTestCompareNearImpl
{
	/** near：逐分量差不超过 tol（向量厘米、旋转度、颜色 0–1）。 */
	FGameTestCompareResult Near(const FGameTestCompareInput& In)
	{
		const bool bHolds = GameTestCompareUtil::ComponentsNear(In.Actual.Value, In.Expected, In.Actual.Kind, In.Tol);
		return GameTestCompareUtil::Result(bHolds, In, *FString::Printf(TEXT("接近（容差 %g）"), In.Tol));
	}
}

namespace GameTestBuiltin
{
	void RegisterCompareNear()
	{
		using K = EGameTestKind;
		FGameTestCompareRegistry::Get().Register(MakeShared<FGameTestCompareFn>(TEXT("near"), TArray<K>{ K::Vector, K::Rotator, K::Vector2, K::Color }, TEXT("接近"),
			TEXT("逐分量差不超过 tol（向量默认 10 厘米、旋转 1 度、颜色每通道 0.02）"), true, &GameTestCompareNearImpl::Near));
	}
}
