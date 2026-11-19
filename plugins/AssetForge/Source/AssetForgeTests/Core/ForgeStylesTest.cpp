// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/ForgeDoc.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgeStyles.h"
#include "Core/ForgeTypes.h"

#if WITH_AUTOMATION_WORKER

/**
 * 样式复用（顶层 styles + 节点 style）的守门用例。
 *
 * 样式在文档解析阶段展开：展开错了就是静默改错资产（漏了样式里的属性、一个节点的覆盖漏到别的节点上），
 * 写回时不还原就是导出一次样式全被抄回每个节点。这里只走纯逻辑入口，不碰资产。
 */
TEST_CLASS_WITH_FLAGS(ForgeStylesTest, "AssetForge.Styles",
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

	/** 一份文档：InStyles 是 styles 的值，InChildren 是根节点 children 数组里的内容。 */
	static FString MakeDocument(const FString& InStyles, const FString& InChildren)
	{
		return FString::Printf(TEXT(R"({
			"kind": "umg-widget",
			"schemaVersion": 1,
			"revision": 1,
			"asset": "/Game/UI/Test/WBP_StyleProbe",
			"styles": %s,
			"root": { "class": "VerticalBox", "name": "Root", "children": [ %s ] }
		})"), *InStyles, *InChildren);
	}

	static const TCHAR* NavStyles()
	{
		return TEXT(R"({
			"Nav": { "props": { "ToolTipText": "nav", "RenderOpacity": 0.5 }, "slot": { "Padding": { "Left": 4, "Top": 2 } } }
		})");
	}

	static bool HasErrorAt(const TArray<FForgeDiagnostic>& InDiagnostics, const FString& InFieldPath)
	{
		for (const FForgeDiagnostic& Diagnostic : InDiagnostics)
		{
			if (Diagnostic.FieldPath == InFieldPath && Diagnostic.Severity == EForgeSeverity::Error)
			{
				return true;
			}
		}
		return false;
	}

	static FString StringField(const TSharedPtr<FJsonObject>& InObject, const TCHAR* InField)
	{
		FString Value;
		if (InObject.IsValid())
		{
			InObject->TryGetStringField(InField, Value);
		}
		return Value;
	}

	static double NumberField(const TSharedPtr<FJsonObject>& InObject, const TCHAR* InField)
	{
		double Value = -1.0;
		if (InObject.IsValid())
		{
			InObject->TryGetNumberField(InField, Value);
		}
		return Value;
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

	/** ToJson 结果里根节点的第 InIndex 个子节点。 */
	static TSharedPtr<FJsonObject> ChildJson(const TSharedPtr<FJsonObject>& InDocument, int32 InIndex)
	{
		const TSharedPtr<FJsonObject> Root = ObjectField(InDocument, TEXT("root"));
		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (!Root.IsValid() || !Root->TryGetArrayField(TEXT("children"), Children) || Children == nullptr || !Children->IsValidIndex(InIndex))
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* Child = nullptr;
		if (!(*Children)[InIndex].IsValid() || !(*Children)[InIndex]->TryGetObject(Child) || Child == nullptr)
		{
			return nullptr;
		}
		return *Child;
	}

	TEST_METHOD(样式展开进节点的props和slot)
	{
		FForgeDocument Document;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsTrue(ParseText(MakeDocument(NavStyles(), TEXT(R"({ "class": "Button", "name": "A", "style": "Nav" })")), Document, Diagnostics),
			TEXT("合法文档应解析成功")));
		ASSERT_THAT(IsTrue(Document.Root.Children.Num() == 1));

		const FForgeNode& Node = Document.Root.Children[0];
		ASSERT_THAT(IsTrue(StringField(Node.Props, TEXT("ToolTipText")) == TEXT("nav"), TEXT("样式里的 props 应展开到节点上")));
		ASSERT_THAT(IsTrue(FMath::IsNearlyEqual(NumberField(Node.Props, TEXT("RenderOpacity")), 0.5), TEXT("样式里的数值属性应展开")));
		ASSERT_THAT(IsTrue(FMath::IsNearlyEqual(NumberField(ObjectField(Node.Slot, TEXT("Padding")), TEXT("Left")), 4.0), TEXT("样式里的 slot 应展开")));
		ASSERT_THAT(IsTrue(Node.Styles.Num() == 1 && Node.Styles[0] == TEXT("Nav"), TEXT("样式名留着，写回时还原成引用")));
	}

	TEST_METHOD(节点自己写的覆盖样式且结构体逐字段合并)
	{
		FForgeDocument Document;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsTrue(ParseText(MakeDocument(NavStyles(), TEXT(R"({
			"class": "Button", "name": "A", "style": "Nav",
			"props": { "RenderOpacity": 1 },
			"slot": { "Padding": { "Left": 8 } }
		})")), Document, Diagnostics)));

		const FForgeNode& Node = Document.Root.Children[0];
		ASSERT_THAT(IsTrue(FMath::IsNearlyEqual(NumberField(Node.Props, TEXT("RenderOpacity")), 1.0), TEXT("节点写的值覆盖样式")));
		ASSERT_THAT(IsTrue(StringField(Node.Props, TEXT("ToolTipText")) == TEXT("nav"), TEXT("节点没写的仍取样式")));
		const TSharedPtr<FJsonObject> Padding = ObjectField(Node.Slot, TEXT("Padding"));
		ASSERT_THAT(IsTrue(FMath::IsNearlyEqual(NumberField(Padding, TEXT("Left")), 8.0), TEXT("结构体里节点写的字段覆盖样式")));
		ASSERT_THAT(IsTrue(FMath::IsNearlyEqual(NumberField(Padding, TEXT("Top")), 2.0), TEXT("结构体里节点没写的字段保留样式的")));
	}

	TEST_METHOD(样式数组后面的覆盖前面的)
	{
		const FString Styles = TEXT(R"({
			"Base": { "props": { "ToolTipText": "base", "RenderOpacity": 0.2 } },
			"Nav": { "props": { "ToolTipText": "nav" } }
		})");
		FForgeDocument Document;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsTrue(ParseText(MakeDocument(Styles, TEXT(R"({ "class": "Button", "name": "A", "style": ["Base", "Nav"] })")), Document, Diagnostics)));

		const FForgeNode& Node = Document.Root.Children[0];
		ASSERT_THAT(IsTrue(StringField(Node.Props, TEXT("ToolTipText")) == TEXT("nav"), TEXT("后面的样式覆盖前面的")));
		ASSERT_THAT(IsTrue(FMath::IsNearlyEqual(NumberField(Node.Props, TEXT("RenderOpacity")), 0.2), TEXT("后面没写的取前面的")));
	}

	TEST_METHOD(字段名写法不同也认成同一个)
	{
		const FString Styles = TEXT(R"({ "Off": { "props": { "bIsEnabled": false } } })");
		FForgeDocument Document;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsTrue(ParseText(MakeDocument(Styles, TEXT(R"({ "class": "Button", "name": "A", "style": "Off", "props": { "IsEnabled": true } })")), Document, Diagnostics)));

		const FForgeNode& Node = Document.Root.Children[0];
		ASSERT_THAT(IsTrue(Node.Props.IsValid() && Node.Props->Values.Num() == 1, TEXT("bIsEnabled 和 IsEnabled 是同一个属性，只留一份")));
		bool bEnabled = false;
		ASSERT_THAT(IsTrue(Node.Props->TryGetBoolField(TEXT("IsEnabled"), bEnabled) && bEnabled, TEXT("留下的是节点写的值")));
	}

	TEST_METHOD(一个节点的覆盖不漏到别的节点)
	{
		FForgeDocument Document;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsTrue(ParseText(MakeDocument(NavStyles(), TEXT(R"(
			{ "class": "Button", "name": "A", "style": "Nav", "props": { "RenderOpacity": 1 } },
			{ "class": "Button", "name": "B", "style": "Nav" }
		)")), Document, Diagnostics)));

		ASSERT_THAT(IsTrue(FMath::IsNearlyEqual(NumberField(Document.Root.Children[1].Props, TEXT("RenderOpacity")), 0.5),
			TEXT("样式对象被几个节点共用，A 的覆盖不能改到 B")));
		const TSharedPtr<FJsonObject> NavProps = ObjectField(ObjectField(Document.Styles, TEXT("Nav")), TEXT("props"));
		ASSERT_THAT(IsTrue(FMath::IsNearlyEqual(NumberField(NavProps, TEXT("RenderOpacity")), 0.5), TEXT("样式定义本身不变")));
	}

	TEST_METHOD(未知样式名报错)
	{
		FForgeDocument Document;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsFalse(ParseText(MakeDocument(NavStyles(), TEXT(R"({ "class": "Button", "name": "A", "style": "Missing" })")), Document, Diagnostics),
			TEXT("引用不存在的样式应失败")));
		ASSERT_THAT(IsTrue(HasErrorAt(Diagnostics, TEXT("root.children[0].style")), TEXT("错误指向节点的 style")));
	}

	TEST_METHOD(样式只能写props和slot)
	{
		FForgeDocument Document;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsFalse(ParseText(MakeDocument(TEXT(R"({ "Nav": { "class": "Button" } })"), TEXT(R"({ "class": "Button", "name": "A" })")), Document, Diagnostics)));
		ASSERT_THAT(IsTrue(HasErrorAt(Diagnostics, TEXT("styles.Nav.class")), TEXT("错误指向样式里多写的字段")));
	}

	TEST_METHOD(style写错类型报错)
	{
		FForgeDocument Document;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsFalse(ParseText(MakeDocument(NavStyles(), TEXT(R"({ "class": "Button", "name": "A", "style": 3 })")), Document, Diagnostics)));
		ASSERT_THAT(IsTrue(HasErrorAt(Diagnostics, TEXT("root.children[0].style"))));
	}

	TEST_METHOD(写回保留样式引用且不重复写相同属性)
	{
		FForgeDocument Document;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsTrue(ParseText(MakeDocument(NavStyles(), TEXT(R"({ "class": "Button", "name": "A", "style": "Nav", "props": { "RenderOpacity": 1 } })")), Document, Diagnostics)));

		const TSharedPtr<FJsonObject> Json = ForgeDoc::ToJson(Document);
		ASSERT_THAT(IsTrue(ObjectField(Json, TEXT("styles")).IsValid(), TEXT("styles 原样写回")));
		const TSharedPtr<FJsonObject> Child = ChildJson(Json, 0);
		ASSERT_THAT(IsTrue(StringField(Child, TEXT("style")) == TEXT("Nav"), TEXT("节点上还原成样式引用")));
		const TSharedPtr<FJsonObject> Props = ObjectField(Child, TEXT("props"));
		ASSERT_THAT(IsTrue(Props.IsValid() && Props->Values.Num() == 1, TEXT("和样式相同的属性不再写，只留覆盖的")));
		ASSERT_THAT(IsTrue(FMath::IsNearlyEqual(NumberField(Props, TEXT("RenderOpacity")), 1.0)));
		ASSERT_THAT(IsFalse(ObjectField(Child, TEXT("slot")).IsValid(), TEXT("slot 和样式完全一样，整个不写")));

		// 写回的再读一遍，展开结果和原来一样
		FForgeDocument Again;
		TArray<FForgeDiagnostic> AgainDiagnostics;
		ASSERT_THAT(IsTrue(ForgeDoc::Parse(Json, TEXT("test.json"), Again, AgainDiagnostics), TEXT("写回的文档能再解析")));
		ASSERT_THAT(IsTrue(ForgeJsonText::ToCanonicalString(Again.Root.Children[0].Props) == ForgeJsonText::ToCanonicalString(Document.Root.Children[0].Props),
			TEXT("往返后展开的 props 一致")));
	}

	TEST_METHOD(导出时搬回原配置的样式)
	{
		FForgeDocument Existing;
		TArray<FForgeDiagnostic> Diagnostics;
		ASSERT_THAT(IsTrue(ParseText(MakeDocument(NavStyles(), TEXT(R"({ "class": "Button", "name": "A", "style": "Nav" })")), Existing, Diagnostics)));

		// 模拟从资产导出的文档：属性是全量的，没有样式信息
		FForgeDocument Exported;
		Exported.Kind = Existing.Kind;
		Exported.SchemaVersion = 1;
		Exported.Asset = Existing.Asset;
		Exported.Root.ClassName = TEXT("VerticalBox");
		Exported.Root.Name = TEXT("Root");
		FForgeNode& Button = Exported.Root.Children.AddDefaulted_GetRef();
		Button.ClassName = TEXT("Button");
		Button.Name = TEXT("A");
		Button.Props = MakeShared<FJsonObject>();
		Button.Props->SetStringField(TEXT("ToolTipText"), TEXT("nav"));
		Button.Props->SetNumberField(TEXT("RenderOpacity"), 0.8);

		ForgeStyles::CarryStyles(Existing, Exported);
		ASSERT_THAT(IsTrue(Exported.Root.Children[0].Styles.Num() == 1, TEXT("按 name 认回原节点的样式引用")));

		const TSharedPtr<FJsonObject> Props = ObjectField(ChildJson(ForgeDoc::ToJson(Exported), 0), TEXT("props"));
		ASSERT_THAT(IsTrue(Props.IsValid() && Props->Values.Num() == 1, TEXT("资产里和样式相同的属性不写")));
		ASSERT_THAT(IsTrue(FMath::IsNearlyEqual(NumberField(Props, TEXT("RenderOpacity")), 0.8), TEXT("资产里改过的写成节点上的覆盖")));
	}
};

#endif // WITH_AUTOMATION_WORKER
