// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/ForgeDoc.h"
#include "Core/ForgeDocSource.h"
#include "Core/ForgeJsonText.h"

#if WITH_AUTOMATION_WORKER

/**
 * include（片段文件）的守门用例：展开、认片段、报错、诊断加文件名、写回保留结构。
 *
 * 文件全在内存里（自造加载器），不碰磁盘。路径一律经 `ResolveIncludePath` 算出来再当表的键，
 * 和真跑时的路径规整一致，不依赖平台的路径写法。
 */
TEST_CLASS_WITH_FLAGS(ForgeDocSourceTest, "AssetForge.Include",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 主文件的路径（绝对路径，规整过）。 */
	static FString MainPath()
	{
		return ForgeDocSource::ResolveIncludePath(TEXT("/ForgeIncludeTest/umg/UI/WBP_Shop.json"), FString());
	}

	/** 相对主文件的片段路径 → 规整后的绝对路径（加载器表的键）。 */
	static FString PathOf(const TCHAR* InRelative)
	{
		return ForgeDocSource::ResolveIncludePath(InRelative, MainPath());
	}

	static ForgeDocSource::FLoader MakeLoader(const TMap<FString, FString>& InFiles)
	{
		return [InFiles](const FString& InPath, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
		{
			const FString* Text = InFiles.Find(InPath);
			if (Text == nullptr)
			{
				OutError = TEXT("文件不存在");
				return false;
			}
			return ForgeJsonText::ParseObject(ForgeJsonText::StripComments(*Text), OutJson, OutError);
		};
	}

	static TSharedPtr<FJsonObject> Parse(const FString& InText)
	{
		TSharedPtr<FJsonObject> Json;
		FString Error;
		ForgeJsonText::ParseObject(InText, Json, Error);
		return Json;
	}

	static bool Expand(const TMap<FString, FString>& InFiles, ForgeDocSource::FExpanded& OutExpanded, TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		const FString* MainText = InFiles.Find(MainPath());
		if (MainText == nullptr)
		{
			return false;
		}
		return ForgeDocSource::Expand(Parse(*MainText), MainPath(), MakeLoader(InFiles), OutExpanded, OutDiagnostics);
	}

	static bool HasDiagnostic(const TArray<FForgeDiagnostic>& InDiagnostics, const FString& InFieldPath, EForgeSeverity InSeverity)
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

	/** 对象里某个数组字段的元素（不存在返回空数组）。 */
	static TArray<TSharedPtr<FJsonValue>> ArrayOf(const TSharedPtr<FJsonObject>& InObject, const TCHAR* InField)
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (InObject.IsValid() && InObject->TryGetArrayField(InField, Items) && Items != nullptr)
		{
			return *Items;
		}
		return {};
	}

	static TSharedPtr<FJsonObject> ObjectOf(const TSharedPtr<FJsonValue>& InValue)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (InValue.IsValid() && InValue->TryGetObject(Object) && Object != nullptr)
		{
			return *Object;
		}
		return nullptr;
	}

	static TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject>& InObject, const TCHAR* InField)
	{
		const TSharedPtr<FJsonObject>* Value = nullptr;
		if (InObject.IsValid() && InObject->TryGetObjectField(InField, Value) && Value != nullptr)
		{
			return *Value;
		}
		return nullptr;
	}

	static FString NameOf(const TSharedPtr<FJsonValue>& InValue)
	{
		FString Name;
		const TSharedPtr<FJsonObject> Object = ObjectOf(InValue);
		if (Object.IsValid())
		{
			Object->TryGetStringField(TEXT("name"), Name);
		}
		return Name;
	}

	/** 主文件：侧栏两个节点在片段里，动画整段在片段里。 */
	static TMap<FString, FString> ShopFiles()
	{
		TMap<FString, FString> Files;
		Files.Add(MainPath(), TEXT(R"({
			"kind": "umg-widget", "schemaVersion": 1, "revision": 1,
			"asset": "/Game/UI/WBP_Shop",
			"root": { "class": "Overlay", "name": "Root", "children": [
				{ "include": "WBP_Shop.parts/Side.json" },
				{ "class": "VerticalBox", "name": "Main" }
			] },
			"animations": { "include": "WBP_Shop.parts/animations.json" }
		})"));
		Files.Add(PathOf(TEXT("WBP_Shop.parts/Side.json")), TEXT(R"({
			"//": "侧栏",
			"fragment": "node",
			"node": [ { "class": "SizeBox", "name": "SideA" }, { "class": "SizeBox", "name": "SideB" } ]
		})"));
		Files.Add(PathOf(TEXT("WBP_Shop.parts/animations.json")), TEXT(R"({
			"fragment": "animations",
			"animations": [ { "name": "PageIn", "tracks": [] } ]
		})"));
		return Files;
	}

	TEST_METHOD(节点include展开成一组节点)
	{
		ForgeDocSource::FExpanded Expanded;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsTrue(Expand(ShopFiles(), Expanded, Diagnostics), TEXT("合法的 include 应展开成功")));

		const TArray<TSharedPtr<FJsonValue>> Children = ArrayOf(ObjectField(Expanded.Json, TEXT("root")), TEXT("children"));
		ASSERT_THAT(IsTrue(Children.Num() == 3, TEXT("片段里的两个节点 + 主文件的一个")));
		ASSERT_THAT(IsTrue(NameOf(Children[0]) == TEXT("SideA") && NameOf(Children[1]) == TEXT("SideB") && NameOf(Children[2]) == TEXT("Main"),
			TEXT("片段的节点按 include 的位置展开")));
		ASSERT_THAT(IsTrue(Expanded.Fragments.Num() == 2, TEXT("用到两个片段")));
		const FString* Origin = Expanded.Origins.Find(TEXT("root.children[1]"));
		ASSERT_THAT(IsTrue(Origin != nullptr && *Origin == TEXT("WBP_Shop.parts/Side.json"), TEXT("记下节点来自哪个片段（相对主文件）")));
	}

	TEST_METHOD(段include整段换成片段内容)
	{
		ForgeDocSource::FExpanded Expanded;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsTrue(Expand(ShopFiles(), Expanded, Diagnostics)));
		ASSERT_THAT(IsTrue(ArrayOf(Expanded.Json, TEXT("animations")).Num() == 1, TEXT("animations 段换成片段里的数组")));
		ASSERT_THAT(IsTrue(Expanded.Origins.Contains(TEXT("animations")), TEXT("记下整段来自片段")));

		// 展开后就是普通文档，解析照旧
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ForgeDoc::Parse(Expanded.Json, MainPath(), Document, Diagnostics), TEXT("展开后的文档能解析")));
		ASSERT_THAT(IsTrue(Document.Root.Children.Num() == 3));
	}

	TEST_METHOD(kind和fragment互斥)
	{
		ForgeDocSource::FExpanded Expanded;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsFalse(ForgeDocSource::Expand(Parse(TEXT(R"({ "kind": "umg-widget", "fragment": "node", "node": {} })")), MainPath(), MakeLoader({}), Expanded, Diagnostics),
			TEXT("同时有 kind 和 fragment 应失败")));
		ASSERT_THAT(IsTrue(HasDiagnostic(Diagnostics, TEXT("fragment"), EForgeSeverity::Error)));

		Diagnostics.Reset();
		ASSERT_THAT(IsFalse(ForgeDocSource::Expand(Parse(TEXT(R"({ "fragment": "node", "node": {} })")), MainPath(), MakeLoader({}), Expanded, Diagnostics),
			TEXT("片段不能当资源配置读")));
		ASSERT_THAT(IsTrue(ForgeDocSource::IsFragment(Parse(TEXT(R"({ "fragment": "node", "node": {} })"))), TEXT("只有 fragment 的是片段")));
		ASSERT_THAT(IsFalse(ForgeDocSource::IsFragment(Parse(TEXT(R"({ "kind": "umg-widget", "fragment": "node" })"))), TEXT("有 kind 的不算片段（扫描时当配置报错）")));
	}

	TEST_METHOD(include资源配置报错)
	{
		TMap<FString, FString> Files = ShopFiles();
		Files.Add(PathOf(TEXT("WBP_Shop.parts/Side.json")), TEXT(R"({ "kind": "umg-widget", "asset": "/Game/UI/WBP_Side", "root": {} })"));
		ForgeDocSource::FExpanded Expanded;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsFalse(Expand(Files, Expanded, Diagnostics), TEXT("include 指向资源配置应失败（那是 ref 的事）")));
		ASSERT_THAT(IsTrue(HasDiagnostic(Diagnostics, TEXT("root.children[0]"), EForgeSeverity::Error), TEXT("错误指向 include 所在位置")));
	}

	TEST_METHOD(片段放错位置报错)
	{
		TMap<FString, FString> Files = ShopFiles();
		Files.Add(MainPath(), TEXT(R"({
			"kind": "umg-widget", "schemaVersion": 1, "asset": "/Game/UI/WBP_Shop",
			"root": { "class": "Overlay", "name": "Root", "children": [ { "include": "WBP_Shop.parts/animations.json" } ] }
		})"));
		ForgeDocSource::FExpanded Expanded;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsFalse(Expand(Files, Expanded, Diagnostics), TEXT("动画片段不能 include 到控件树里")));
		ASSERT_THAT(IsTrue(HasDiagnostic(Diagnostics, TEXT("root.children[0]"), EForgeSeverity::Error)));
	}

	TEST_METHOD(include成环报错)
	{
		TMap<FString, FString> Files;
		Files.Add(MainPath(), TEXT(R"({
			"kind": "umg-widget", "schemaVersion": 1, "asset": "/Game/UI/WBP_Shop",
			"root": { "class": "Overlay", "name": "Root", "children": [ { "include": "Loop.json" } ] }
		})"));
		Files.Add(PathOf(TEXT("Loop.json")), TEXT(R"({
			"fragment": "node",
			"node": { "class": "SizeBox", "name": "Loop", "children": [ { "include": "Loop.json" } ] }
		})"));
		ForgeDocSource::FExpanded Expanded;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsFalse(Expand(Files, Expanded, Diagnostics), TEXT("片段 include 自己应失败")));
		ASSERT_THAT(IsTrue(HasDiagnostic(Diagnostics, TEXT("root.children[0].children[0]"), EForgeSeverity::Error), TEXT("错误指向成环的那个 include")));
	}

	TEST_METHOD(include不能带别的字段)
	{
		TMap<FString, FString> Files = ShopFiles();
		Files.Add(MainPath(), TEXT(R"({
			"kind": "umg-widget", "schemaVersion": 1, "asset": "/Game/UI/WBP_Shop",
			"root": { "class": "Overlay", "name": "Root", "children": [ { "include": "WBP_Shop.parts/Side.json", "name": "X" } ] }
		})"));
		ForgeDocSource::FExpanded Expanded;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsFalse(Expand(Files, Expanded, Diagnostics)));
		ASSERT_THAT(IsTrue(HasDiagnostic(Diagnostics, TEXT("root.children[0]"), EForgeSeverity::Error)));
	}

	TEST_METHOD(同一片段引用两次给警告)
	{
		TMap<FString, FString> Files = ShopFiles();
		Files.Add(MainPath(), TEXT(R"({
			"kind": "umg-widget", "schemaVersion": 1, "asset": "/Game/UI/WBP_Shop",
			"root": { "class": "Overlay", "name": "Root", "children": [
				{ "include": "WBP_Shop.parts/Side.json" },
				{ "include": "WBP_Shop.parts/Side.json" }
			] }
		})"));
		ForgeDocSource::FExpanded Expanded;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsTrue(Expand(Files, Expanded, Diagnostics), TEXT("重复引用只警告不拦")));
		ASSERT_THAT(IsTrue(HasDiagnostic(Diagnostics, TEXT("root.children[2]"), EForgeSeverity::Warning), TEXT("第二次引用处给警告")));
	}

	TEST_METHOD(诊断加上片段文件名)
	{
		TMap<FString, FString> Origins;
		Origins.Add(TEXT("animations"), TEXT("WBP_Shop.parts/animations.json"));
		Origins.Add(TEXT("root.children[1]"), TEXT("WBP_Shop.parts/Side.json"));

		TArray<FForgeDiagnostic> Diagnostics;
		Diagnostics.Emplace(EForgeSeverity::Error, TEXT("animations[0].tracks[0].channel"), TEXT("x"));
		Diagnostics.Emplace(EForgeSeverity::Error, TEXT("root.children[1].props.Text"), TEXT("x"));
		Diagnostics.Emplace(EForgeSeverity::Error, TEXT("root.children[10].props.Text"), TEXT("x"));
		ForgeDocSource::LabelDiagnostics(Origins, Diagnostics, 0);

		ASSERT_THAT(IsTrue(Diagnostics[0].FieldPath == TEXT("WBP_Shop.parts/animations.json: animations[0].tracks[0].channel")));
		ASSERT_THAT(IsTrue(Diagnostics[1].FieldPath == TEXT("WBP_Shop.parts/Side.json: root.children[1].props.Text")));
		ASSERT_THAT(IsTrue(Diagnostics[2].FieldPath == TEXT("root.children[10].props.Text"), TEXT("children[1] 不是 children[10] 的前缀")));
	}

	TEST_METHOD(写回保留include结构)
	{
		const TMap<FString, FString> Files = ShopFiles();
		// 新文档：SideA 改了属性、Main 后面新加一个节点、动画改了
		const TSharedPtr<FJsonObject> NewDocument = Parse(TEXT(R"({
			"kind": "umg-widget", "schemaVersion": 1, "revision": 1,
			"asset": "/Game/UI/WBP_Shop",
			"root": { "class": "Overlay", "name": "Root", "children": [
				{ "class": "SizeBox", "name": "SideA", "props": { "WidthOverride": 200 } },
				{ "class": "SizeBox", "name": "SideB" },
				{ "class": "VerticalBox", "name": "Main" },
				{ "class": "TextBlock", "name": "Added" }
			] },
			"animations": [ { "name": "PageIn", "tracks": [] }, { "name": "PageOut", "tracks": [] } ]
		})"));

		TArray<ForgeDocSource::FFileWrite> Writes;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsTrue(ForgeDocSource::Collapse(NewDocument, MainPath(), MakeLoader(Files), Writes, Diagnostics)));
		ASSERT_THAT(IsTrue(Writes.Num() == 3 && Writes[0].Path == MainPath(), TEXT("主文件 + 两个片段，主文件在第一个")));

		const TSharedPtr<FJsonObject> Main = Writes[0].Json;
		const TArray<TSharedPtr<FJsonValue>> MainChildren = ArrayOf(ObjectField(Main, TEXT("root")), TEXT("children"));
		ASSERT_THAT(IsTrue(MainChildren.Num() == 3, TEXT("include、Main、新加的节点")));
		ASSERT_THAT(IsTrue(ObjectOf(MainChildren[0]).IsValid() && ObjectOf(MainChildren[0])->HasField(TEXT("include")), TEXT("侧栏仍是 include")));
		ASSERT_THAT(IsTrue(NameOf(MainChildren[2]) == TEXT("Added"), TEXT("新增的节点写在主文件")));
		ASSERT_THAT(IsTrue(ObjectField(Main, TEXT("animations")).IsValid(), TEXT("动画段仍是 include（对象），不是数组")));

		const ForgeDocSource::FFileWrite* Side = Writes.FindByPredicate([](const ForgeDocSource::FFileWrite& Write) { return Write.Path == PathOf(TEXT("WBP_Shop.parts/Side.json")); });
		ASSERT_THAT(IsTrue(Side != nullptr && Side->bFragment, TEXT("侧栏片段要写")));
		const TArray<TSharedPtr<FJsonValue>> SideNodes = ArrayOf(Side->Json, TEXT("node"));
		ASSERT_THAT(IsTrue(SideNodes.Num() == 2 && NameOf(SideNodes[0]) == TEXT("SideA"), TEXT("片段里还是那两个节点")));
		ASSERT_THAT(IsTrue(ObjectField(ObjectOf(SideNodes[0]), TEXT("props")).IsValid(), TEXT("SideA 的改动写进片段")));
		FString Comment;
		ASSERT_THAT(IsTrue(Side->Json->TryGetStringField(TEXT("//"), Comment), TEXT("片段原来的 // 注释保留")));

		const ForgeDocSource::FFileWrite* Animations = Writes.FindByPredicate([](const ForgeDocSource::FFileWrite& Write) { return Write.Path == PathOf(TEXT("WBP_Shop.parts/animations.json")); });
		ASSERT_THAT(IsTrue(Animations != nullptr && ArrayOf(Animations->Json, TEXT("animations")).Num() == 2, TEXT("新动画写进动画片段")));
	}

	TEST_METHOD(没有原文件时整份写主文件)
	{
		const TSharedPtr<FJsonObject> NewDocument = Parse(TEXT(R"({ "kind": "umg-widget", "schemaVersion": 1, "asset": "/Game/UI/WBP_New", "root": { "class": "Overlay", "name": "Root" } })"));
		TArray<ForgeDocSource::FFileWrite> Writes;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsTrue(ForgeDocSource::Collapse(NewDocument, MainPath(), MakeLoader({}), Writes, Diagnostics)));
		ASSERT_THAT(IsTrue(Writes.Num() == 1 && Writes[0].Path == MainPath() && !Writes[0].bFragment));
	}
};

#endif // WITH_AUTOMATION_WORKER
