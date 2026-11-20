// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Compare/GameTestCompareRegistry.h"

#include "Observe/Ref/GameTestPropPath.h"

namespace GameTestCompareVisibilityImpl
{
	/** ESlateVisibility 的成员名（去掉 `ESlateVisibility::`）。 */
	FString NameOf(const FGameTestValue& InValue)
	{
		FString Name = GameTestCompareUtil::TextOf(InValue);
		int32 Colon = INDEX_NONE;
		return Name.FindLastChar(TEXT(':'), Colon) ? Name.Mid(Colon + 1) : Name;
	}

	bool IsVisibleName(const FString& InName)
	{
		return InName.Equals(TEXT("Visible"), ESearchCase::IgnoreCase) || InName.Equals(TEXT("HitTestInvisible"), ESearchCase::IgnoreCase)
			|| InName.Equals(TEXT("SelfHitTestInvisible"), ESearchCase::IgnoreCase);
	}

	FGameTestCompareResult Visible(const FGameTestCompareInput& In)
	{
		return GameTestCompareUtil::Result(IsVisibleName(NameOf(In.Actual.Value)), In, TEXT("可见"));
	}

	FGameTestCompareResult Hidden(const FGameTestCompareInput& In)
	{
		const FString Name = NameOf(In.Actual.Value);
		return GameTestCompareUtil::Result(Name.Equals(TEXT("Collapsed"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("Hidden"), ESearchCase::IgnoreCase), In, TEXT("隐藏"));
	}
}

namespace GameTestBuiltin
{
	void RegisterCompareVisibility()
	{
		using namespace GameTestCompareVisibilityImpl;
		using K = EGameTestKind;
		TGameTestRegistry<IGameTestCompare>& Registry = FGameTestCompareRegistry::Get();
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("visible"), TArray<K>{ K::Visibility }, TEXT("可见"),
			TEXT("Visible / HitTestInvisible / SelfHitTestInvisible 任一"), false, &Visible));
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("hidden"), TArray<K>{ K::Visibility }, TEXT("隐藏"),
			TEXT("Collapsed / Hidden 任一"), false, &Hidden));
	}
}
