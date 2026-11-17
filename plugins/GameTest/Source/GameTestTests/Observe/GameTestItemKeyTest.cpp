// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/GameTestJson.h"
#include "Observe/GameTestItemKey.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_AUTOMATION_WORKER

namespace
{
	TSharedPtr<FJsonObject> ParseObject(const FString& InJson)
	{
		FString Error;
		return GameTestJson::ParseObjectFromText(InJson, Error);
	}

	double ScoreOf(const FString& InSource, const FString& InPath, const TSharedRef<FJsonValue>& InValue, bool bInUnique = true, bool bInChanged = false)
	{
		FString Why;
		return GameTestItemKey::Score(InSource, InPath, *InValue, bInUnique, /*bInEqualsIndex=*/false, bInChanged, /*bInLocalizedText=*/false, Why);
	}
}

TEST_CLASS_WITH_FLAGS(GameTestItemKeyTest, "GameTest.ItemKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	TEST_METHOD(ParseConds_NumberIsIndexNotCondition)
	{
		TArray<FGameTestKeyCond> Conds;
		FString Error;
		ASSERT_THAT(IsFalse(GameTestItemKey::ParseConds(TEXT("3"), Conds, Error), TEXT("[3] 是序号，不是条件")));
		ASSERT_THAT(IsTrue(Error.IsEmpty(), TEXT("序号不算写错，不该带错误")));
	}

	TEST_METHOD(ParseConds_SourcesAndCombos)
	{
		TArray<FGameTestKeyCond> Conds;
		FString Error;
		ASSERT_THAT(IsTrue(GameTestItemKey::ParseConds(TEXT("Data.ItemId=1001, @lua.Slot=3"), Conds, Error), *Error));
		ASSERT_THAT(AreEqual(2, Conds.Num()));
		ASSERT_THAT(AreEqual(FString(), Conds[0].Source, TEXT("不写来源用默认来源")));
		ASSERT_THAT(AreEqual(FString(TEXT("Data.ItemId")), Conds[0].Path));
		ASSERT_THAT(AreEqual(FString(TEXT("1001")), Conds[0].Value));
		ASSERT_THAT(AreEqual(FString(TEXT("lua")), Conds[1].Source));
		ASSERT_THAT(AreEqual(FString(TEXT("Slot")), Conds[1].Path));
	}

	TEST_METHOD(ParseConds_RejectsUnknownSourceAndMissingValue)
	{
		TArray<FGameTestKeyCond> Conds;
		FString Error;
		ASSERT_THAT(IsFalse(GameTestItemKey::ParseConds(TEXT("@foo.Id=1"), Conds, Error), TEXT("@foo 不是来源")));
		ASSERT_THAT(IsFalse(Error.IsEmpty(), TEXT("不认识的来源要说清楚")));
		Error.Reset();
		ASSERT_THAT(IsFalse(GameTestItemKey::ParseConds(TEXT("ItemId"), Conds, Error), TEXT("没有值不是条件")));
		ASSERT_THAT(IsFalse(Error.IsEmpty(), TEXT("没有 = 要说清楚怎么写")));
	}

	TEST_METHOD(FormatValue_IntegersAndUnsafeText)
	{
		ASSERT_THAT(AreEqual(FString(TEXT("1001")), GameTestItemKey::FormatValue(FJsonValueNumber(1001.0)), TEXT("整数不带小数点")));
		ASSERT_THAT(AreEqual(FString(TEXT("true")), GameTestItemKey::FormatValue(FJsonValueBoolean(true))));
		ASSERT_THAT(AreEqual(FString(TEXT("Sword")), GameTestItemKey::FormatValue(FJsonValueString(TEXT("Sword")))));
		ASSERT_THAT(IsTrue(GameTestItemKey::FormatValue(FJsonValueString(TEXT("a,b"))).IsEmpty(), TEXT("带逗号的写不进方括号")));
		ASSERT_THAT(IsTrue(GameTestItemKey::FormatValue(FJsonValueString(TEXT("A>B"))).IsEmpty(), TEXT("带 > 的写不进选择器")));
	}

	TEST_METHOD(Score_IdOnDataItemBeatsPositionAndFloat)
	{
		const double Id = ScoreOf(TEXT("item"), TEXT("ItemId"), MakeShared<FJsonValueNumber>(1001.0));
		const double Index = ScoreOf(TEXT("item"), TEXT("SlotIndex"), MakeShared<FJsonValueNumber>(4.0));
		const double Float = ScoreOf(TEXT("item"), TEXT("Weight"), MakeShared<FJsonValueNumber>(1.5));
		ASSERT_THAT(IsTrue(Id >= GameTestItemKey::ConfidentScore, TEXT("数据对象上唯一的整数 Id 应该够准")));
		ASSERT_THAT(IsTrue(Id > Index, TEXT("像位置的字段排在 Id 后面")));
		ASSERT_THAT(IsTrue(Index > Float, TEXT("小数基本不能当标识")));
	}

	TEST_METHOD(Score_ChangedOrNotUniqueSinks)
	{
		const double Good = ScoreOf(TEXT("item"), TEXT("ItemId"), MakeShared<FJsonValueNumber>(1001.0));
		const double Changed = ScoreOf(TEXT("item"), TEXT("ItemId"), MakeShared<FJsonValueNumber>(1001.0), true, /*bInChanged=*/true);
		const double Shared = ScoreOf(TEXT("item"), TEXT("ItemId"), MakeShared<FJsonValueNumber>(1001.0), /*bInUnique=*/false);
		ASSERT_THAT(IsTrue(Changed < 0.0 && Changed < Good, TEXT("点一下就变的值不能用")));
		ASSERT_THAT(IsTrue(Shared < Changed, TEXT("不唯一比会变更不能用")));
	}

	TEST_METHOD(IsConfident_NeedsUniqueUnchangedHighScore)
	{
		TArray<FGameTestKeyCandidate> Sorted;
		FGameTestKeyCandidate Top;
		Top.Score = 7.0;
		Top.Hits = 1;
		Sorted.Add(Top);
		ASSERT_THAT(IsTrue(GameTestItemKey::IsConfident(Sorted), TEXT("唯一、没变、7 分应该够准")));
		Sorted[0].bChanged = true;
		ASSERT_THAT(IsFalse(GameTestItemKey::IsConfident(Sorted), TEXT("会变的不算准")));
		Sorted[0].bChanged = false;
		Sorted[0].Score = 3.0;
		ASSERT_THAT(IsFalse(GameTestItemKey::IsConfident(Sorted), TEXT("分数不够要问人")));
	}

	TEST_METHOD(FindPath_DottedAndCaseInsensitive)
	{
		const TSharedPtr<FJsonObject> Object = ParseObject(TEXT(R"({"Data":{"ItemId":1001,"Name":"剑"}})"));
		ASSERT_THAT(IsTrue(Object.IsValid(), TEXT("测试数据要能解析")));
		const TSharedPtr<FJsonValue> Value = GameTestItemKey::FindPath(*Object, TEXT("data.itemid"));
		ASSERT_THAT(IsTrue(Value.IsValid(), TEXT("路径大小写不敏感")));
		// CQTest 不让 AreEqual 比浮点：Id 是整数，直接比相等
		ASSERT_THAT(IsTrue(Value->AsNumber() == 1001.0, TEXT("取到的值应是 1001")));
		ASSERT_THAT(IsFalse(GameTestItemKey::FindPath(*Object, TEXT("Data.Missing")).IsValid(), TEXT("没有的字段取不到")));
	}

	TEST_METHOD(Flatten_OnlyScalarsWithPaths)
	{
		const TSharedPtr<FJsonObject> Object = ParseObject(TEXT(R"({"Id":1,"Data":{"Cfg":7,"Tags":[1,2]},"Name":"甲"})"));
		ASSERT_THAT(IsTrue(Object.IsValid(), TEXT("测试数据要能解析")));
		TArray<TPair<FString, TSharedPtr<FJsonValue>>> Leaves;
		GameTestItemKey::Flatten(*Object, 3, 100, Leaves);
		TArray<FString> Paths;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Leaf : Leaves)
		{
			Paths.Add(Leaf.Key);
		}
		ASSERT_THAT(IsTrue(Paths.Contains(TEXT("Id")) && Paths.Contains(TEXT("Data.Cfg")) && Paths.Contains(TEXT("Name")), TEXT("标量都要摊平出来")));
		ASSERT_THAT(IsFalse(Paths.Contains(TEXT("Data.Tags")), TEXT("数组不当标识")));
	}
};

#endif
