// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/ForgeDoc.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgeTypes.h"
#include "Kinds/Umg/UmgForge.h"

#if WITH_AUTOMATION_WORKER

/**
 * UMG 处理器的守门用例。
 *
 * 测的是「通用层给不了、只有这个 kind 才知道」的判定：控件名在 UMG 里是导入时的
 * **匹配键**，所以兄弟重名在这里是硬错误（通用层只给警告——行为树的节点名就只是标签）。
 *
 * 走 dry-run 而不是真导入：`ImportDocument` 在 dry-run 下做完校验就返回，
 * 不会去解析父类、不会建包、更不会写盘，所以用例不依赖任何已有资产。
 */
TEST_CLASS_WITH_FLAGS(UmgForgeTest, "AssetForge.Umg",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 造一份最小文档，只换 root 那一段。 */
	static bool ParseDocument(const FString& InRootBody, FForgeDocument& OutDocument)
	{
		const FString Text = FString::Printf(TEXT(R"({
			"kind": "umg-widget",
			"schemaVersion": 1,
			"revision": 1,
			"asset": "/Game/UI/Test/WBP_ForgeProbe",
			"parent": "/Script/UMG.UserWidget",
			"root": %s
		})"), *InRootBody);

		TSharedPtr<FJsonObject> Json;
		FString Error;
		if (!ForgeJsonText::ParseObject(Text, Json, Error))
		{
			return false;
		}

		TArray<FForgeDiagnostic> Diagnostics;
		return ForgeDoc::Parse(Json, TEXT("test.json"), OutDocument, Diagnostics);
	}

	static bool HasErrorAt(const FForgeReport& InReport, const FString& InFieldPath)
	{
		for (const FForgeDiagnostic& Diagnostic : InReport.Diagnostics)
		{
			if (Diagnostic.FieldPath == InFieldPath && Diagnostic.Severity == EForgeSeverity::Error)
			{
				return true;
			}
		}
		return false;
	}

	TEST_METHOD(兄弟重名被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(TEXT(R"({
			"class": "VerticalBox",
			"name": "Root",
			"children": [
				{ "class": "TextBlock", "name": "Label" },
				{ "class": "TextBlock", "name": "Label" }
			]
		})"), Document), TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		const bool bSucceeded = UmgForge::ImportDocument(Document, /*bDryRun=*/true, Report);

		ASSERT_THAT(IsFalse(bSucceeded, TEXT("重名会让匹配键失去唯一性，UMG 侧必须拒绝")));
		ASSERT_THAT(IsTrue(HasErrorAt(Report, TEXT("root.children")),
			TEXT("诊断应是 error 级别并指向出问题的 children")));
	}

	TEST_METHOD(不同父节点下的同名控件通过)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(TEXT(R"({
			"class": "HorizontalBox",
			"name": "Root",
			"children": [
				{ "class": "VerticalBox", "name": "Left", "children": [ { "class": "TextBlock", "name": "Label" } ] },
				{ "class": "VerticalBox", "name": "Right", "children": [ { "class": "TextBlock", "name": "Label" } ] }
			]
		})"), Document), TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		const bool bSucceeded = UmgForge::ImportDocument(Document, /*bDryRun=*/true, Report);

		ASSERT_THAT(IsTrue(bSucceeded, TEXT("重名只在同一父节点下才破坏唯一性")));
	}
};

#endif // WITH_AUTOMATION_WORKER
