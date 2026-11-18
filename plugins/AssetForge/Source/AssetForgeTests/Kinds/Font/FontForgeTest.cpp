// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"
#include "ForgeTestAssets.h"

#include "Registry/ForgeKindRegistry.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgePipeline.h"
#include "Core/ForgeTypes.h"
#include "Kinds/Font/FontForge.h"

#include "Engine/Font.h"
#include "Engine/FontFace.h"

#if WITH_AUTOMATION_WORKER

/**
 * 字体处理器的守门用例。
 *
 * 测的是「这个 kind 才知道、通用层给不了」的判定：
 *
 * - `UFont` 被分派到 font；`UFontFace` **刻意不被分派**（它是附属资产，纳管入口是 UFont）；
 * - `FontCacheType` 必须显式写 `Runtime`（CDO 默认 Offline，省略会被重置成离线烘焙模式）；
 * - 至少一个字体面；面的 `source` / `typeface` 是必需字段，源文件必须真实存在；
 * - 重名（面名与 typeface 名）被拒绝——两者分别是匹配键与 Slate 的选择键；
 * - 结构属性（`CompositeFont`）与源文件属性（`SourceFilename`）不能走 props。
 *
 * 走 dry-run 与纯函数：不建包、不写盘。源文件用的是仓库里真实存在的 ttf
 * （SourceArt/Font/ 下由配置纳管的那批），与用例所处的项目一致。
 */
TEST_CLASS_WITH_FLAGS(FontForgeTest, "AssetForge.Font",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 仓库里真实存在的源文件（相对项目根）。 */
	static const TCHAR* const ProbeSource()
	{
		return TEXT("SourceArt/Font/MapleMono-NF-CN-Regular.ttf");
	}

	/** 造一份最小文档，只换 root 那一段；asset 指向一个必定不存在的路径——dry-run 不碰盘。 */
	static bool ParseDocument(const FString& InRootBody, FForgeDocument& OutDocument)
	{
		const FString Text = FString::Printf(TEXT(R"({
			"kind": "font",
			"schemaVersion": 1,
			"revision": 1,
			"asset": "/Game/Test/Font/F_ForgeProbe",
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

	static bool HasDiagnosticAt(const FForgeReport& InReport, const FString& InFieldPath, EForgeSeverity InSeverity)
	{
		for (const FForgeDiagnostic& Diagnostic : InReport.Diagnostics)
		{
			if (Diagnostic.FieldPath == InFieldPath && Diagnostic.Severity == InSeverity)
			{
				return true;
			}
		}
		return false;
	}

	static FString MakeRoot(const FString& InClass, const FString& InProps, const FString& InChildren)
	{
		return FString::Printf(TEXT(R"({
			"class": "%s",
			"name": "F_ForgeProbe",
			"props": %s,
			"children": %s
		})"), *InClass, *InProps, *InChildren);
	}

	static FString MakeFace(const FString& InName, const FString& InTypeface, const FString& InSource, const FString& InProps = TEXT("{}"))
	{
		return FString::Printf(TEXT(R"({
			"class": "FontFace",
			"name": "%s",
			"typeface": "%s",
			"source": "%s",
			"props": %s
		})"), *InName, *InTypeface, *InSource, *InProps);
	}

	/** 一份合法的最小配置：Runtime + 一个面。 */
	static FString MakeValidRoot()
	{
		return MakeRoot(
			TEXT("Font"),
			TEXT(R"({ "FontCacheType": "Runtime" })"),
			FString::Printf(TEXT("[ %s ]"), *MakeFace(TEXT("FF_ForgeProbe_Regular"), TEXT("Regular"), ProbeSource())));
	}

	TEST_METHOD(字体资产被分派到_font)
	{
		// 这条接线断了，资产右键菜单的"导入/导出配置"与导出分派都会失灵。
		ASSERT_THAT(AreEqual(FString(FontForge::Kind), ForgeKindRegistry::ClassifyAssetClass(UFont::StaticClass()),
			TEXT("UFont 必须被分派到 font")));

		const FString FaceKind = ForgeKindRegistry::ClassifyAssetClass(UFontFace::StaticClass());
		ASSERT_THAT(IsTrue(FaceKind.IsEmpty(),
			TEXT("UFontFace 是附属资产，刻意不单独认领 kind")));
	}

	TEST_METHOD(类型已登记在管道里)
	{
		ASSERT_THAT(IsTrue(ForgeKindRegistry::GetSupportedKinds().Contains(FontForge::Kind),
			TEXT("font 必须出现在支持的 kind 名单里（窗口与校验都读它）")));
	}

	TEST_METHOD(FontCacheType_必须显式写_Runtime)
	{
		// 缺省 → 报错：CDO 默认是 Offline，省略会被重置成离线烘焙模式（那是另一条完全不同的管线）。
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("Font"),
			TEXT("{}"),
			FString::Printf(TEXT("[ %s ]"), *MakeFace(TEXT("FF_ForgeProbe_Regular"), TEXT("Regular"), ProbeSource()))), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(FontForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("省略 FontCacheType 必须拒绝")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.props.FontCacheType"), EForgeSeverity::Error),
			TEXT("诊断应指向 root.props.FontCacheType")));
	}

	TEST_METHOD(Class_必须是_UFont)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("Texture2D"),
			TEXT(R"({ "FontCacheType": "Runtime" })"),
			FString::Printf(TEXT("[ %s ]"), *MakeFace(TEXT("FF_ForgeProbe_Regular"), TEXT("Regular"), ProbeSource()))), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(FontForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("别的资产类不能被当成字体")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.class"), EForgeSeverity::Error),
			TEXT("诊断应指向 root.class")));
	}

	TEST_METHOD(至少需要一个字体面)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("Font"),
			TEXT(R"({ "FontCacheType": "Runtime" })"),
			TEXT("[]")), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(FontForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("空字体没有可表达的内容，必须拒绝")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.children"), EForgeSeverity::Error),
			TEXT("诊断应指向 root.children")));
	}

	TEST_METHOD(字体面必须写_source)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("Font"),
			TEXT(R"({ "FontCacheType": "Runtime" })"),
			FString::Printf(TEXT("[ %s ]"), *MakeFace(TEXT("FF_ForgeProbe_Regular"), TEXT("Regular"), TEXT("")))), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(FontForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("缺 source：面的内容无从谈起，必须拒绝")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.children[0].source"), EForgeSeverity::Error),
			TEXT("诊断应指向 root.children[0].source")));
	}

	TEST_METHOD(不存在的源文件被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("Font"),
			TEXT(R"({ "FontCacheType": "Runtime" })"),
			FString::Printf(TEXT("[ %s ]"), *MakeFace(TEXT("FF_ForgeProbe_Regular"), TEXT("Regular"), TEXT("SourceArt/Font/NoSuchFont.ttf")))), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(FontForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("源文件不存在必须报错，不能静默跳过")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.children[0].source"), EForgeSeverity::Error),
			TEXT("诊断应指向 root.children[0].source")));
	}

	TEST_METHOD(字体面必须写_typeface)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("Font"),
			TEXT(R"({ "FontCacheType": "Runtime" })"),
			FString::Printf(TEXT("[ %s ]"), *MakeFace(TEXT("FF_ForgeProbe_Regular"), TEXT(""), ProbeSource()))), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(FontForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("缺 typeface：UI 里没法选择这一面，必须拒绝")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.children[0].typeface"), EForgeSeverity::Error),
			TEXT("诊断应指向 root.children[0].typeface")));
	}

	TEST_METHOD(typeface_重名被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("Font"),
			TEXT(R"({ "FontCacheType": "Runtime" })"),
			FString::Printf(TEXT("[ %s, %s ]"),
				*MakeFace(TEXT("FF_ForgeProbe_Regular"), TEXT("Regular"), ProbeSource()),
				*MakeFace(TEXT("FF_ForgeProbe_Bold"), TEXT("Regular"), ProbeSource()))), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(FontForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("两个同名 entry 在 UFont 里无法区分，必须拒绝")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.children"), EForgeSeverity::Error),
			TEXT("诊断应指向 root.children")));
	}

	TEST_METHOD(props_里写_CompositeFont_被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("Font"),
			TEXT(R"({ "FontCacheType": "Runtime", "CompositeFont": {} })"),
			FString::Printf(TEXT("[ %s ]"), *MakeFace(TEXT("FF_ForgeProbe_Regular"), TEXT("Regular"), ProbeSource()))), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(FontForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("结构由 children 表达，props 里直接写复合字体必须拒绝")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.props.CompositeFont"), EForgeSeverity::Error),
			TEXT("诊断应指向 root.props.CompositeFont")));
	}

	TEST_METHOD(字体面_props_里写_SourceFilename_被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("Font"),
			TEXT(R"({ "FontCacheType": "Runtime" })"),
			FString::Printf(TEXT("[ %s ]"), *MakeFace(
				TEXT("FF_ForgeProbe_Regular"),
				TEXT("Regular"),
				ProbeSource(),
				TEXT(R"({ "SourceFilename": "SourceArt/Font/Other.ttf" })")))), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(FontForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("源文件由 source 字段表达，props 里再写一份必须拒绝")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.children[0].props.SourceFilename"), EForgeSeverity::Error),
			TEXT("诊断应指向 root.children[0].props.SourceFilename")));
	}

	TEST_METHOD(属性名写错被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("Font"),
			TEXT(R"({ "FontCacheType": "Runtime", "NoSuchSetting": 1 })"),
			FString::Printf(TEXT("[ %s ]"), *MakeFace(TEXT("FF_ForgeProbe_Regular"), TEXT("Regular"), ProbeSource()))), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(FontForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("写错的属性名不能静默通过")));
		ASSERT_THAT(IsTrue(Report.Diagnostics.Num() > 0, TEXT("至少给出一条诊断")));
	}

	TEST_METHOD(目标不存在时计划是创建)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeValidRoot(), Document), TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsTrue(FontForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("目标不存在 + 合法配置时应给出创建计划")));
		ASSERT_THAT(AreEqual(2, Report.Actions.Num(), TEXT("一条根 + 一条字体面")));
		ASSERT_THAT(IsTrue(Report.Actions.Num() > 0 && Report.Actions[0].Action == EForgeAction::Create
			&& Report.Actions[0].NodePath == TEXT("root"), TEXT("第一条是根创建")));
		ASSERT_THAT(IsTrue(Report.Actions.Num() > 1 && Report.Actions[1].Action == EForgeAction::Create
			&& Report.Actions[1].NodeName == TEXT("FF_ForgeProbe_Regular"), TEXT("第二条是字体面创建")));
	}

	TEST_METHOD(关键参数与结构总是出现在导出里)
	{
		FORGE_SKIP_WITHOUT_ASSET(ForgeTestAssets::Font);
		// 对项目里真实的字体资产走完整导出：16 个字体面、每个都带 source/typeface，
		// 根 props 显式写 FontCacheType——这条守着"导出方向永远写全关键参数"的约定，
		// 也覆盖"导入的资产能被完整读回来"（纳管闭环）。
		FForgeDocument Document;
		FForgeReport Report;
		ASSERT_THAT(IsTrue(ForgePipeline::Export(
			ForgeTestAssets::Font, Document, Report),
			TEXT("导出真实字体应成功")));

		ASSERT_THAT(IsTrue(Document.Root.Props.IsValid() && Document.Root.Props->HasField(TEXT("FontCacheType")),
			TEXT("FontCacheType 必须显式出现在导出的 props 里")));
		ASSERT_THAT(AreEqual(16, Document.Root.Children.Num(), TEXT("16 个字重/样式各一个字体面")));

		TSet<FString> FoundTypefaces;
		for (const FForgeNode& Child : Document.Root.Children)
		{
			FString Source;
			FString Typeface;
			if (Child.Extra.IsValid())
			{
				Child.Extra->TryGetStringField(TEXT("source"), Source);
				Child.Extra->TryGetStringField(TEXT("typeface"), Typeface);
			}

			ASSERT_THAT(IsTrue(Source.StartsWith(TEXT("SourceArt/Font/")),
				FString::Printf(TEXT("字体面 %s 的 source 应指回 SourceArt/Font（实际 %s）"), *Child.Name, *Source)));
			ASSERT_THAT(IsTrue(!Typeface.IsEmpty(),
				FString::Printf(TEXT("字体面 %s 应带 typeface"), *Child.Name)));
			FoundTypefaces.Add(Typeface);
		}

		const TCHAR* const ExpectedTypefaces[] = {
			TEXT("Regular"), TEXT("Italic"), TEXT("Bold"), TEXT("BoldItalic"),
			TEXT("Thin"), TEXT("ExtraLight"), TEXT("Light"), TEXT("Medium"),
			TEXT("SemiBold"), TEXT("ExtraBold"),
		};
		for (const TCHAR* Expected : ExpectedTypefaces)
		{
			ASSERT_THAT(IsTrue(FoundTypefaces.Contains(Expected),
				FString::Printf(TEXT("应能导出 typeface %s"), Expected)));
		}
	}
};

#endif // WITH_AUTOMATION_WORKER
