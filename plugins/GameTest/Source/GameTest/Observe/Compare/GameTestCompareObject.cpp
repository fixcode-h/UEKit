// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Compare/GameTestCompareRegistry.h"

#include "Observe/Ref/GameTestPropPath.h"
#include "Observe/Sources/GameTestObjectRoots.h"

#include "UObject/Class.h"

namespace GameTestCompareObjectImpl
{
	using namespace GameTestCompareUtil;

	bool IsEmptyValue(const FGameTestValue& InValue)
	{
		return InValue.IsNull() || (InValue.IsString() && InValue.GetString().IsEmpty()) || (InValue.Type == EGameTestValueType::Object && InValue.GetObject() == nullptr);
	}

	/** 值是类（`:class`、类引用）时给类，是对象时给它的类；类引用读成名字时按名字找。 */
	const UClass* ClassOf(const FGameTestValue& InValue)
	{
		if (const UObject* Object = InValue.GetObject())
		{
			const UClass* AsClass = Cast<UClass>(Object);
			return AsClass != nullptr ? AsClass : Object->GetClass();
		}
		return InValue.IsString() && !InValue.GetString().IsEmpty() ? GameTestObjectRoots::FindClass(InValue.GetString()) : nullptr;
	}

	FGameTestCompareResult IsNull(const FGameTestCompareInput& In)
	{
		return Result(IsEmptyValue(In.Actual.Value), In, TEXT("为空"));
	}

	FGameTestCompareResult NotNull(const FGameTestCompareInput& In)
	{
		return Result(!IsEmptyValue(In.Actual.Value), In, TEXT("不为空"));
	}

	FGameTestCompareResult ClassIs(const FGameTestCompareInput& In)
	{
		const UClass* Class = ClassOf(In.Actual.Value);
		return Result(Class != nullptr && GameTestObjectRoots::ClassMatches(Class, TextOf(In.Expected)), In, TEXT("的类是（按继承）"));
	}

	FGameTestCompareResult NameIs(const FGameTestCompareInput& In)
	{
		const UObject* Object = In.Actual.Value.GetObject();
		FString Name = Object != nullptr ? Object->GetName() : TextOf(In.Actual.Value);
		FString Expected = TextOf(In.Expected);
		Name.RemoveFromEnd(TEXT("_C"));
		Expected.RemoveFromEnd(TEXT("_C"));
		return Result(Name.Equals(Expected, ESearchCase::IgnoreCase), In, TEXT("的名字是"));
	}
}

namespace GameTestBuiltin
{
	void RegisterCompareObject()
	{
		using namespace GameTestCompareObjectImpl;
		using K = EGameTestKind;
		TGameTestRegistry<IGameTestCompare>& Registry = FGameTestCompareRegistry::Get();
		const FGameTestCompareFn::FValidateFn Text = [](const TSharedPtr<FJsonValue>& V, FGameTestDiagSink& S) { GameTestCompareUtil::ExpectString(V, S); };
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("isNull"), TArray<K>{ K::Object, K::Asset }, TEXT("为空"), TEXT("引用是空的"), false, &IsNull));
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("notNull"), TArray<K>{ K::Object, K::Asset }, TEXT("不为空"), TEXT("引用不是空的"), false, &NotNull));
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("classIs"), TArray<K>{ K::Object }, TEXT("的类是"), TEXT("对象的类按继承认（子类也算）"), true, &ClassIs, Text));
		Registry.Register(MakeShared<FGameTestCompareFn>(TEXT("nameIs"), TArray<K>{ K::Object }, TEXT("的名字是"), TEXT("对象名（忽略大小写和 _C）"), true, &NameIs, Text));
	}
}
