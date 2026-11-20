// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/Expr/GameTestExpr.h"
#include "Core/GameTestJson.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Observe/Compare/GameTestCompareRegistry.h"
#include "Observe/Compare/GameTestJudge.h"
#include "Observe/Ref/GameTestPropPath.h"
#include "Observe/Ref/GameTestRef.h"
#include "Record/V4/GameTestRecordV4.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_AUTOMATION_WORKER

namespace GameTestRefTestLocal
{
	TSharedPtr<FJsonObject> Parse(const TCHAR* InText)
	{
		FString Error;
		return GameTestJson::ParseObjectFromText(InText, Error);
	}

	bool HasCode(const FGameTestDiagSink& InSink, const TCHAR* InCode)
	{
		return InSink.GetAll().ContainsByPredicate([InCode](const FGameTestDiag& Diag) { return Diag.Code == FName(InCode); });
	}

	FString Describe(const FGameTestDiagSink& InSink)
	{
		TArray<FString> Lines;
		for (const FGameTestDiag& Diag : InSink.GetAll())
		{
			Lines.Add(Diag.Code.ToString() + TEXT(" ") + Diag.ToLine());
		}
		return FString::Join(Lines, TEXT("；"));
	}

	/** 一条判断对着现成的值比（不碰游戏对象）。 */
	bool Holds(const FGameTestValue& InValue, EGameTestKind InKind, const TCHAR* InItem)
	{
		FGameTestPropRead Read;
		Read.Value = InValue;
		Read.Kind = InKind;
		const TSharedPtr<FJsonObject> Json = Parse(InItem);
		FGameTestFlowContext Context;
		return GameTestJudge::TestRead(Read, FGameTestIdentItem::FromJson(*Json), Context).bHolds;
	}
}

TEST_CLASS_WITH_FLAGS(GameTestRefTest, "GameTest.Ref",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 属性路径：@来源.路径、@部件:状态、:状态；不以 @ / : 开头的拒绝。 */
	TEST_METHOD(PropPath_Parse)
	{
		FGameTestPropPath Path;
		FString Error;
		ASSERT_THAT(IsTrue(FGameTestPropPath::Parse(TEXT("@self.ShopId"), Path, Error), Error));
		ASSERT_THAT(AreEqual(2, Path.Segments.Num()));
		ASSERT_THAT(AreEqual(FString(TEXT("ShopId")), Path.Segments[1]));
		ASSERT_THAT(IsTrue(FGameTestPropPath::Parse(TEXT(":onScreen"), Path, Error), Error));
		ASSERT_THAT(AreEqual(FString(TEXT("onScreen")), Path.State));
		ASSERT_THAT(IsTrue(FGameTestPropPath::Parse(TEXT("@Header.TxtTitle:text"), Path, Error), Error));
		ASSERT_THAT(AreEqual(FString(TEXT("text")), Path.State));
		ASSERT_THAT(IsFalse(FGameTestPropPath::Parse(TEXT("ShopId"), Path, Error)));
	}

	/** 比较按 kind 给 op：文字没有 gt，数字没有 contains；visibility 有 visible。 */
	TEST_METHOD(Compare_OpsForKind)
	{
		const TArray<FString> StringOps = FGameTestCompareRegistry::OpsFor(EGameTestKind::String);
		ASSERT_THAT(IsTrue(StringOps.Contains(TEXT("contains"))));
		ASSERT_THAT(IsFalse(StringOps.Contains(TEXT("gt"))));
		const TArray<FString> IntOps = FGameTestCompareRegistry::OpsFor(EGameTestKind::Int);
		ASSERT_THAT(IsTrue(IntOps.Contains(TEXT("between"))));
		ASSERT_THAT(IsFalse(IntOps.Contains(TEXT("contains"))));
		ASSERT_THAT(IsTrue(FGameTestCompareRegistry::OpsFor(EGameTestKind::Visibility).Contains(TEXT("visible"))));
	}

	/** 判断项对着值比：eq 忽略大小写（默认）、contains、数字范围。 */
	TEST_METHOD(Judge_TestRead)
	{
		using namespace GameTestRefTestLocal;
		ASSERT_THAT(IsTrue(Holds(FGameTestValue::FromString(TEXT("购买成功")), EGameTestKind::String, TEXT(R"({"prop":":text","op":"contains","value":"购买"})"))));
		ASSERT_THAT(IsFalse(Holds(FGameTestValue::FromString(TEXT("金币不足")), EGameTestKind::String, TEXT(R"({"prop":":text","op":"contains","value":"购买"})"))));
		ASSERT_THAT(IsTrue(Holds(FGameTestValue::FromString(TEXT("Shop")), EGameTestKind::String, TEXT(R"({"prop":"@self.Name","op":"eq","value":"shop"})"))));
		ASSERT_THAT(IsTrue(Holds(FGameTestValue::FromNumber(5), EGameTestKind::Int, TEXT(R"({"prop":"@self.N","op":"between","value":[1,10]})"))));
		ASSERT_THAT(IsFalse(Holds(FGameTestValue::FromNumber(11), EGameTestKind::Int, TEXT(R"({"prop":"@self.N","op":"between","value":[1,10]})"))));
	}

	/** objects 的写法检查：选择器字符串报 selector_removed，in.obj 成环报 ref_cycle。 */
	TEST_METHOD(RefCheck_ObjectsAndSelectors)
	{
		using namespace GameTestRefTestLocal;
		FGameTestExprScope Scope;
		FGameTestDiagSink Cycle;
		GameTestRefCheck::Objects(*Parse(TEXT(R"({"a":{"class":"WBP_A","in":{"obj":"b","part":"List"}},"b":{"class":"WBP_B","in":{"obj":"a","part":"List"}}})")), Scope, Cycle);
		ASSERT_THAT(IsTrue(HasCode(Cycle, TEXT("ref_cycle")), Describe(Cycle)));

		FGameTestDiagSink Selector;
		GameTestRefCheck::Target(MakeShared<FJsonValueString>(TEXT("WBP_Shop:Root/BtnBuy")), Scope, Selector);
		ASSERT_THAT(IsTrue(HasCode(Selector, TEXT("selector_removed")), Describe(Selector)));

		FGameTestDiagSink Good;
		const TSharedPtr<FJsonObject> Target = Parse(TEXT(R"({"obj":{"class":"WBP_Shop","ident":[{"prop":"@self.ShopId","op":"eq","value":1001}]},"part":"BtnBuy"})"));
		GameTestRefCheck::Target(MakeShared<FJsonValueObject>(Target), Scope, Good);
		ASSERT_THAT(IsFalse(Good.HasErrors(), Describe(Good)));
	}

	/** 录制的选择器 → v4：屏去重进 objects，条目 → in + ident，嵌套 WBP 用 `.` 连。 */
	TEST_METHOD(RecordV4_TargetOf)
	{
		using namespace GameTestRefTestLocal;
		FJsonObject Objects;
		FString Todo;
		const TSharedPtr<FJsonObject> Plain = GameTestRecordV4::TargetOf(TEXT("WBP_Shop:Root/Panel/BtnBuy"), nullptr, Objects, Todo);
		ASSERT_THAT(AreEqual(FString(TEXT("shop")), Plain->GetStringField(TEXT("obj"))));
		ASSERT_THAT(AreEqual(FString(TEXT("BtnBuy")), Plain->GetStringField(TEXT("part"))));
		ASSERT_THAT(IsTrue(Objects.HasField(TEXT("shop"))));
		ASSERT_THAT(IsTrue(Todo.IsEmpty(), Todo));

		const TSharedPtr<FJsonObject> Nested = GameTestRecordV4::TargetOf(TEXT("WBP_Shop:Root/Header>BtnClose"), nullptr, Objects, Todo);
		ASSERT_THAT(AreEqual(FString(TEXT("Header.BtnClose")), Nested->GetStringField(TEXT("part"))));

		const TSharedPtr<FJsonObject> Key = Parse(TEXT(R"({"list":true,"entry":"WBP_BagSlot_C"})"));
		const TSharedPtr<FJsonObject> Entry = GameTestRecordV4::TargetOf(TEXT("WBP_Bag:Root/List[Data.ItemId=1001]>BtnUse"), Key, Objects, Todo);
		ASSERT_THAT(AreEqual(FString(TEXT("BtnUse")), Entry->GetStringField(TEXT("part"))));
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		ASSERT_THAT(IsTrue(Entry->TryGetObjectField(TEXT("obj"), Obj) && Obj != nullptr));
		ASSERT_THAT(AreEqual(FString(TEXT("WBP_BagSlot")), (*Obj)->GetStringField(TEXT("class"))));
		const TArray<TSharedPtr<FJsonValue>>* Ident = nullptr;
		ASSERT_THAT(IsTrue((*Obj)->TryGetArrayField(TEXT("ident"), Ident) && Ident != nullptr && Ident->Num() == 1));
		const TSharedPtr<FJsonObject> First = (*Ident)[0].IsValid() ? (*Ident)[0]->AsObject() : nullptr;
		ASSERT_THAT(IsTrue(First.IsValid()));
		ASSERT_THAT(AreEqual(FString(TEXT("@item.Data.ItemId")), First->GetStringField(TEXT("prop"))));
		ASSERT_THAT(IsTrue(FMath::IsNearlyEqual(First->GetNumberField(TEXT("value")), 1001.0)));

		// 按第几个认的：many first + todo
		FString IndexTodo;
		const TSharedPtr<FJsonObject> ByIndex = GameTestRecordV4::TargetOf(TEXT("WBP_Bag:Root/List[2]>BtnUse"), Key, Objects, IndexTodo);
		ASSERT_THAT(IsFalse(IndexTodo.IsEmpty()));
	}

	/** 录到的 assert（target + visible / text）改写成 cond 里的对象判断。 */
	TEST_METHOD(RecordV4_ConvertAssert)
	{
		using namespace GameTestRefTestLocal;
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"({"action":"assert","target":"WBP_Shop:Root/TxtTitle","visible":true,"text":"杂货铺"})"))));
		Steps.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"({"action":"click","target":"WBP_Shop:Root/BtnBuy","skipIf":{"target":"WBP_Shop:Root/ChkAuto","checked":true,"also":1}})"))));
		FJsonObject Objects;
		ASSERT_THAT(AreEqual(0, GameTestRecordV4::Convert(Steps, Objects)));
		const TSharedPtr<FJsonObject> AssertStep = Steps[0]->AsObject();
		ASSERT_THAT(IsTrue(AssertStep.IsValid()));
		ASSERT_THAT(IsFalse(AssertStep->HasField(TEXT("visible"))));
		const TSharedPtr<FJsonObject> Cond = GameTestJson::TryGetObject(*AssertStep, TEXT("cond"));
		ASSERT_THAT(IsTrue(Cond.IsValid()));
		// 部件并进属性路径（判断里没有 part）
		ASSERT_THAT(AreEqual(FString(TEXT("@TxtTitle:text")), Cond->GetStringField(TEXT("prop"))));
		ASSERT_THAT(IsFalse(Cond->HasField(TEXT("part"))));
		const TSharedPtr<FJsonObject> Click = Steps[1]->AsObject();
		ASSERT_THAT(IsTrue(Click.IsValid()));
		const TSharedPtr<FJsonObject> SkipIf = GameTestJson::TryGetObject(*Click, TEXT("skipIf"));
		ASSERT_THAT(IsTrue(SkipIf.IsValid() && SkipIf->HasField(TEXT("cond")) && SkipIf->HasField(TEXT("also"))));
		const TSharedPtr<FJsonObject> SkipCond = GameTestJson::TryGetObject(*SkipIf, TEXT("cond"));
		ASSERT_THAT(IsTrue(SkipCond.IsValid()));
		ASSERT_THAT(AreEqual(FString(TEXT("@ChkAuto:checked")), SkipCond->GetStringField(TEXT("prop"))));
	}
};

#endif
