// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/GameTestJson.h"
#include "Observe/GameTestItemMatch.h"
#include "Observe/GameTestWidgetNode.h"

#if WITH_AUTOMATION_WORKER

namespace
{
	/** 造一条列表数据项：`propsJson` 是它的反射属性 dump（与运行时 dump 的形状一致）。 */
	TSharedPtr<FGameTestListItem> MakeItem(int32 InIndex, const FString& InPropsJson)
	{
		FString Error;
		const TSharedPtr<FGameTestListItem> Item = MakeShared<FGameTestListItem>();
		Item->Index = InIndex;
		Item->ClassName = TEXT("GameTestProbeItem");
		Item->Properties = GameTestJson::ParseObjectFromText(InPropsJson, Error);
		return Item;
	}

	/** 造一个期望值（就是步骤里 `equals` 那一侧）。 */
	TSharedPtr<FJsonValue> MakeExpected(const FString& InJson)
	{
		// 借 steps 的解析走一遍：把 JSON 片段包成对象再取字段，避免自己造 FJsonValue 的构造分支。
		const FString Wrapped = FString::Printf(TEXT("{\"value\":%s}"), *InJson);
		FString Error;
		const TSharedPtr<FJsonObject> Object = GameTestJson::ParseObjectFromText(Wrapped, Error);
		return Object.IsValid() ? Object->TryGetField(TEXT("value")) : nullptr;
	}
}

TEST_CLASS_WITH_FLAGS(GameTestItemMatchTest, "GameTest.ItemMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	TEST_METHOD(ValuesEqual_ComparesNumbersNumerically)
	{
		const TSharedPtr<FJsonValue> Left = MakeExpected(TEXT("1"));
		const TSharedPtr<FJsonValue> Right = MakeExpected(TEXT("1.0"));
		ASSERT_THAT(IsTrue(Left.IsValid() && Right.IsValid(), TEXT("期望值要能解析出来")));
		ASSERT_THAT(IsTrue(GameTestItemMatch::ValuesEqual(*Left, *Right), TEXT("1 与 1.0 应视为相等")));
	}

	TEST_METHOD(ValuesEqual_FallsBackToTextWhenTypesDiffer)
	{
		ASSERT_THAT(IsTrue(GameTestItemMatch::ValuesEqual(*MakeExpected(TEXT("\"1\"")), *MakeExpected(TEXT("1"))),
			TEXT("\"1\" 与 1 应命中（配置里两种写法都会出现）")));
	}

	TEST_METHOD(ValuesEqual_StringCompareIgnoresCase)
	{
		ASSERT_THAT(IsTrue(GameTestItemMatch::ValuesEqual(*MakeExpected(TEXT("\"Start\"")), *MakeExpected(TEXT("\"start\""))),
			TEXT("字符串比较应忽略大小写")));
	}

	TEST_METHOD(FindProperty_IgnoresCase)
	{
		const TSharedPtr<FGameTestListItem> Item = MakeItem(0, TEXT(R"({"Id":1,"DisplayName":"甲"})"));
		ASSERT_THAT(IsTrue(GameTestItemMatch::FindProperty(*Item->Properties, TEXT("id")).IsValid(),
			TEXT("id 应能命中 Id")));
		ASSERT_THAT(IsTrue(GameTestItemMatch::FindProperty(*Item->Properties, TEXT("displayname")).IsValid(),
			TEXT("displayname 应能命中 DisplayName")));
	}

	TEST_METHOD(FindIndexByProperty_HitsMatchingItem)
	{
		TArray<TSharedPtr<FGameTestListItem>> Items;
		Items.Add(MakeItem(0, TEXT(R"({"Id":1,"DisplayName":"甲"})")));
		Items.Add(MakeItem(1, TEXT(R"({"Id":2,"DisplayName":"乙"})")));
		Items.Add(MakeItem(2, TEXT(R"({"Id":3,"DisplayName":"丙"})")));

		ASSERT_THAT(IsTrue(GameTestItemMatch::FindIndexByProperty(Items, TEXT("Id"), *MakeExpected(TEXT("2"))) == 1,
			TEXT("Id=2 应命中下标 1")));
		ASSERT_THAT(IsTrue(GameTestItemMatch::FindIndexByProperty(Items, TEXT("DisplayName"), *MakeExpected(TEXT("\"丙\""))) == 2,
			TEXT("DisplayName=丙 应命中下标 2")));
	}

	TEST_METHOD(FindIndexByProperty_ReturnsNoneWhenNoMatch)
	{
		TArray<TSharedPtr<FGameTestListItem>> Items;
		Items.Add(MakeItem(0, TEXT(R"({"Id":1})")));

		ASSERT_THAT(IsTrue(GameTestItemMatch::FindIndexByProperty(Items, TEXT("Id"), *MakeExpected(TEXT("42"))) == INDEX_NONE,
			TEXT("没有 Id=42 的条目")));
		ASSERT_THAT(IsTrue(GameTestItemMatch::FindIndexByProperty(Items, TEXT("NoSuchProperty"), *MakeExpected(TEXT("1"))) == INDEX_NONE,
			TEXT("没有这个属性时应判未命中")));
	}

	TEST_METHOD(DescribeProperty_ListsActualValues)
	{
		TArray<TSharedPtr<FGameTestListItem>> Items;
		Items.Add(MakeItem(0, TEXT(R"({"Id":1})")));
		Items.Add(MakeItem(1, TEXT(R"({"Id":2})")));

		const FString Description = GameTestItemMatch::DescribeProperty(Items, TEXT("Id"), /*InLimit=*/10);
		ASSERT_THAT(IsTrue(Description.Contains(TEXT("Id=1")), *Description));
		ASSERT_THAT(IsTrue(Description.Contains(TEXT("Id=2")), *Description));
	}

	TEST_METHOD(DescribeProperty_ExplainsMissingProperty)
	{
		TArray<TSharedPtr<FGameTestListItem>> Items;
		Items.Add(MakeItem(0, TEXT(R"({"Id":1})")));

		const FString Description = GameTestItemMatch::DescribeProperty(Items, TEXT("Missing"), /*InLimit=*/10);
		ASSERT_THAT(IsTrue(Description.Contains(TEXT("Missing")), *Description));
	}
};

#endif	// WITH_AUTOMATION_WORKER
