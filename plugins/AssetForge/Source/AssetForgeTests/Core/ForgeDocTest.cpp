// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/ForgeDoc.h"
#include "Core/ForgeJsonText.h"

#if WITH_AUTOMATION_WORKER

/**
 * 文档解析的守门用例。
 *
 * 这里拦住的每一条都是「不拦就会静默改错资产」的错误：版本不认识时猜测迁移、
 * 必需字段缺失时用空值继续跑。兄弟重名只给**警告**——通用层不知道各 kind 拿
 * `name` 当什么用（UMG 当匹配键、行为树只当标签），把它当硬错误的那一侧在各自处理器里。
 */
TEST_CLASS_WITH_FLAGS(ForgeDocTest, "AssetForge.Doc",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	static bool ParseText(const FString& InText, FForgeDocument& OutDocument, TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		TSharedPtr<FJsonObject> Json;
		FString Error;
		if (!ForgeJsonText::ParseObject(InText, Json, Error))
		{
			return false;
		}
		return ForgeDoc::Parse(Json, TEXT("test.json"), OutDocument, OutDiagnostics);
	}

	/** 最小的合法文档，用例只改自己关心的那一处。 */
	static FString MakeMinimalDocument(const FString& InRootBody)
	{
		return FString::Printf(TEXT(R"({
			"kind": "umg-widget",
			"schemaVersion": 1,
			"revision": 1,
			"asset": "/Game/UI/Test/WBP_Test",
			"root": %s
		})"), *InRootBody);
	}

	static bool HasDiagnosticAt(const TArray<FForgeDiagnostic>& InDiagnostics, const FString& InFieldPath)
	{
		for (const FForgeDiagnostic& Diagnostic : InDiagnostics)
		{
			if (Diagnostic.FieldPath == InFieldPath)
			{
				return true;
			}
		}
		return false;
	}

	/** 指定路径上有没有指定级别的诊断——重名这类「只提醒不拦」的情况要靠它区分。 */
	static bool HasDiagnosticAt(const TArray<FForgeDiagnostic>& InDiagnostics, const FString& InFieldPath, EForgeSeverity InSeverity)
	{
		for (const FForgeDiagnostic& Diagnostic : InDiagnostics)
		{
			if (Diagnostic.FieldPath == InFieldPath && Diagnostic.Severity == InSeverity)
			{
				return true;
			}
		}
		return false;
	}

	TEST_METHOD(最小文档可解析)
	{
		FForgeDocument Document;
		TArray<FForgeDiagnostic> Diagnostics;

		ASSERT_THAT(IsTrue(ParseText(MakeMinimalDocument(TEXT(R"({ "class": "CanvasPanel", "name": "Root" })")),
			Document, Diagnostics), TEXT("合法文档应解析成功")));
		ASSERT_THAT(IsTrue(Document.Kind == TEXT("umg-widget"), TEXT("kind 应被读出")));
		ASSERT_THAT(IsTrue(Document.Asset == TEXT("/Game/UI/Test/WBP_Test"), TEXT("asset 应被读出")));
		ASSERT_THAT(IsTrue(Document.Root.Name == TEXT("Root"), TEXT("根节点名应被读出")));
	}

	TEST_METHOD(缺少schemaVersion时拒绝加载)
	{
		const FString Text = TEXT(R"({
			"kind": "umg-widget",
			"asset": "/Game/UI/Test/WBP_Test",
			"root": { "class": "CanvasPanel", "name": "Root" }
		})");

		FForgeDocument Document;
		TArray<FForgeDiagnostic> Diagnostics;

		ASSERT_THAT(IsTrue(!ParseText(Text, Document, Diagnostics), TEXT("缺 schemaVersion 应判定失败")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Diagnostics, TEXT("schemaVersion")),
			TEXT("诊断的字段路径应指向 schemaVersion")));
	}

	TEST_METHOD(版本号不认识时拒绝而不是猜测)
	{
		const FString Text = TEXT(R"({
			"kind": "umg-widget",
			"schemaVersion": 99,
			"asset": "/Game/UI/Test/WBP_Test",
			"root": { "class": "CanvasPanel", "name": "Root" }
		})");

		FForgeDocument Document;
		TArray<FForgeDiagnostic> Diagnostics;

		ASSERT_THAT(IsTrue(!ParseText(Text, Document, Diagnostics), TEXT("未知版本应判定失败")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Diagnostics, TEXT("schemaVersion")),
			TEXT("诊断应指向 schemaVersion")));
		ASSERT_THAT(IsTrue(Diagnostics.Num() > 0 && Diagnostics[0].Message.Contains(TEXT("99")),
			TEXT("诊断里应说出实际拿到的版本号，便于人直接改")));
	}

	TEST_METHOD(同一父节点下重名只给警告)
	{
		const FString Text = MakeMinimalDocument(TEXT(R"({
			"class": "VerticalBox",
			"name": "Root",
			"children": [
				{ "class": "TextBlock", "name": "Label" },
				{ "class": "TextBlock", "name": "Label" }
			]
		})"));

		FForgeDocument Document;
		TArray<FForgeDiagnostic> Diagnostics;

		// 通用层不知道 name 在各 kind 里意味着什么（UMG 是匹配键、行为树只是标签），
		// 所以这里只警告；把重名当硬错误的是 UMG 处理器（见 UmgForgeTest）。
		ASSERT_THAT(IsTrue(ParseText(Text, Document, Diagnostics),
			TEXT("通用层不该因为重名拒绝解析")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Diagnostics, TEXT("root.children"), EForgeSeverity::Warning),
			TEXT("重名应以警告形式指出，且指向出问题的 children")));
	}

	TEST_METHOD(不同父节点下同名控件是允许的)
	{
		const FString Text = MakeMinimalDocument(TEXT(R"({
			"class": "HorizontalBox",
			"name": "Root",
			"children": [
				{ "class": "VerticalBox", "name": "Left", "children": [ { "class": "TextBlock", "name": "Label" } ] },
				{ "class": "VerticalBox", "name": "Right", "children": [ { "class": "TextBlock", "name": "Label" } ] }
			]
		})"));

		FForgeDocument Document;
		TArray<FForgeDiagnostic> Diagnostics;

		ASSERT_THAT(IsTrue(ParseText(Text, Document, Diagnostics),
			TEXT("名字只需在同一父节点下唯一")));
	}

	TEST_METHOD(节点缺少class时给出字段级错误)
	{
		const FString Text = MakeMinimalDocument(TEXT(R"({ "name": "Root" })"));

		FForgeDocument Document;
		TArray<FForgeDiagnostic> Diagnostics;

		ASSERT_THAT(IsTrue(!ParseText(Text, Document, Diagnostics), TEXT("缺 class 应判定失败")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Diagnostics, TEXT("root.class")),
			TEXT("诊断路径应精确到 root.class")));
	}

	TEST_METHOD(未识别字段在往返中保留)
	{
		const FString Text = TEXT(R"({
			"kind": "umg-widget",
			"schemaVersion": 1,
			"revision": 3,
			"asset": "/Game/UI/Test/WBP_Test",
			"note": "给未来的自己留句话",
			"root": { "class": "CanvasPanel", "name": "Root" }
		})");

		FForgeDocument Document;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsTrue(ParseText(Text, Document, Diagnostics), TEXT("带额外字段的文档仍应解析成功")));

		const TSharedPtr<FJsonObject> RoundTripped = ForgeDoc::ToJson(Document);
		ASSERT_THAT(IsTrue(RoundTripped->HasField(TEXT("note")),
			TEXT("未识别字段必须原样保留，否则导出会悄悄吃掉作者写的东西")));
	}

	TEST_METHOD(文档往返后结构一致)
	{
		const FString Text = MakeMinimalDocument(TEXT(R"({
			"class": "CanvasPanel",
			"name": "Root",
			"children": [
				{ "class": "TextBlock", "name": "Title", "props": { "RenderOpacity": 0.5 } }
			]
		})"));

		FForgeDocument First;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsTrue(ParseText(Text, First, Diagnostics), TEXT("首轮解析应成功")));

		FForgeDocument Second;
		TArray<FForgeDiagnostic> SecondDiagnostics;
		const FString Serialized = ForgeJsonText::ToCondensedString(ForgeDoc::ToJson(First));
		ASSERT_THAT(IsTrue(ParseText(Serialized, Second, SecondDiagnostics), TEXT("回写的文本应能再次解析")));

		ASSERT_THAT(IsTrue(Second.Root.Children.Num() == 1, TEXT("子节点数应保持")));
		ASSERT_THAT(IsTrue(Second.Root.Children[0].Name == TEXT("Title"), TEXT("子节点名应保持")));
		ASSERT_THAT(IsTrue(Second.Root.Children[0].Props.IsValid()
			&& Second.Root.Children[0].Props->GetNumberField(TEXT("RenderOpacity")) == 0.5,
			TEXT("属性应保持")));
	}
};

#endif // WITH_AUTOMATION_WORKER
