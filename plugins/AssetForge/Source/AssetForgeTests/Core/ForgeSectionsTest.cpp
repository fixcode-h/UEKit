// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "AssetForgeCompat.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgeSections.h"
#include "Registry/ForgeKindSchema.h"

#if WITH_AUTOMATION_WORKER

/**
 * 配置按段排序、按段取的守门用例。
 *
 * 写回时顶层字段的顺序是给人和版本管理看的：顺序乱了，导出一次整份文件都显示成改动；
 * 按段取是 Agent 读长配置的入口，取错段等于给它看了别的东西。
 */
TEST_CLASS_WITH_FLAGS(ForgeSectionsTest, "AssetForge.Sections",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	static TSharedPtr<FJsonObject> Parse(const FString& InText)
	{
		TSharedPtr<FJsonObject> Json;
		FString Error;
		ForgeJsonText::ParseObject(InText, Json, Error);
		return Json;
	}

	static TArray<FString> KeysOf(const TSharedPtr<FJsonObject>& InObject)
	{
		TArray<FString> Keys;
		if (InObject.IsValid())
		{
			for (const auto& Pair : InObject->Values)
			{
				Keys.Add(ASSETFORGE_JSON_KEY(Pair.Key));
			}
		}
		return Keys;
	}

	/** 顺序故意打乱的一份 UMG 配置。 */
	static TSharedPtr<FJsonObject> Shuffled()
	{
		return Parse(TEXT(R"({
			"animations": [],
			"root": { "class": "CanvasPanel", "name": "Root" },
			"asset": "/Game/UI/WBP_X",
			"styles": {},
			"kind": "umg-widget",
			"note": "小字段",
			"//": "注释",
			"$schema": "../.schema/umg-widget.schema.json",
			"revision": 3,
			"schemaVersion": 1,
			"parent": "/Script/UMG.UserWidget"
		})"));
	}

	TEST_METHOD(kind登记的段)
	{
		const TArray<FString> Umg = ForgeKindSchema::GetBodySections(TEXT("umg-widget"));
		ASSERT_THAT(IsTrue(Umg.Num() == 3 && Umg[0] == TEXT("styles") && Umg[1] == TEXT("root") && Umg[2] == TEXT("animations"),
			TEXT("UMG：styles → root → animations")));
		const TArray<FString> Table = ForgeKindSchema::GetBodySections(TEXT("data-table"));
		ASSERT_THAT(IsTrue(Table.Num() == 2 && Table[1] == TEXT("root"), TEXT("没登记大段的 kind 只有 styles、root")));
	}

	TEST_METHOD(写回按段顺序排顶层字段)
	{
		const TSharedPtr<FJsonObject> Ordered = ForgeSections::Reorder(Shuffled(), ForgeKindSchema::GetBodySections(TEXT("umg-widget")));
		const TArray<FString> Expected = {
			TEXT("$schema"), TEXT("//"), TEXT("kind"), TEXT("schemaVersion"), TEXT("revision"), TEXT("asset"), TEXT("parent"),
			TEXT("note"), TEXT("styles"), TEXT("root"), TEXT("animations")};
		const TArray<FString> Actual = KeysOf(Ordered);
		ASSERT_THAT(IsTrue(Actual == Expected, FString::Printf(TEXT("实际顺序：%s"), *FString::Join(Actual, TEXT(", ")))));
		ASSERT_THAT(IsTrue(ForgeJsonText::ToCanonicalString(Ordered) == ForgeJsonText::ToCanonicalString(Shuffled()), TEXT("只换顺序，内容不动")));
	}

	TEST_METHOD(按段取出一段)
	{
		const TArray<FString> Body = ForgeKindSchema::GetBodySections(TEXT("umg-widget"));
		const TSharedPtr<FJsonObject> Animations = ForgeSections::Extract(Shuffled(), TEXT("animations"), Body);
		ASSERT_THAT(IsTrue(KeysOf(Animations) == TArray<FString>({TEXT("animations")}), TEXT("取出的是 { animations: … }")));

		const TSharedPtr<FJsonObject> Header = ForgeSections::Extract(Shuffled(), ForgeSections::HeaderSection, Body);
		ASSERT_THAT(IsTrue(Header.IsValid() && Header->HasField(TEXT("kind")) && Header->HasField(TEXT("note")), TEXT("头段有 kind 和其余小字段")));
		ASSERT_THAT(IsFalse(Header->HasField(TEXT("root")), TEXT("头段不含大段")));

		ASSERT_THAT(IsFalse(ForgeSections::Extract(Shuffled(), TEXT("nope"), Body).IsValid(), TEXT("没有的段取不到")));
		ASSERT_THAT(IsTrue(ForgeSections::ListPresent(Shuffled(), Body).Num() == 4, TEXT("header、styles、root、animations")));
	}

	TEST_METHOD(schema列出段)
	{
		const TSharedPtr<FJsonObject> Schema = ForgeKindSchema::Build(TEXT("umg-widget"));
		const TSharedPtr<FJsonObject>* Hints = nullptr;
		ASSERT_THAT(IsTrue(Schema.IsValid() && Schema->TryGetObjectField(TEXT("x-forge"), Hints) && Hints != nullptr));
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		ASSERT_THAT(IsTrue((*Hints)->TryGetArrayField(TEXT("sections"), Sections) && Sections != nullptr && Sections->Num() == 4,
			TEXT("x-forge.sections：header、styles、root、animations")));
		ASSERT_THAT(IsTrue(ForgeKindSchema::BuildFragment().IsValid(), TEXT("片段也有 schema")));
	}
};

#endif // WITH_AUTOMATION_WORKER
