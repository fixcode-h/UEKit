// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"
#include "ForgeTestAssets.h"

#include "Registry/ForgeKindRegistry.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgePipeline.h"
#include "Core/ForgeTypes.h"
#include "Kinds/Texture/TextureForge.h"

#include "Engine/Texture2D.h"

#if WITH_AUTOMATION_WORKER

/**
 * 纹理处理器的守门用例。
 *
 * 测的是「这个 kind 才知道、通用层给不了」的判定：
 *
 * - `UTexture2D` 必须被分派到 texture（右键菜单与导出分派都读它）；
 * - 纹理配置没有子节点：写了 `children` 要在计划阶段拦下；
 * - `root.class` 必须是 `UTexture2D` 家族；
 * - 源图契约：新纹理必须写 `source`、写了的必须真的在磁盘上；
 * - 目标不存在 + 合法源图时，计划是**创建**。
 *
 * 走 dry-run 与纯函数：不建包、不写盘。「导入出来的纹理能不能用」由端到端回环负责。
 * 源图用的是仓库里真实存在的那张（SourceArt 下的 LUT），与用例所处的项目一致。
 */
TEST_CLASS_WITH_FLAGS(TextureForgeTest, "AssetForge.Texture",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 仓库里真实存在的源图（相对项目根）；自检回环用的也是这张。 */
	static const TCHAR* const ProbeSource()
	{
		return TEXT("SourceArt/Atmosphere/T_Atmosphere_RedDust_LUT.tga");
	}

	/** 造一份最小文档，只换 root 那一段；asset 指向一个必定不存在的路径——dry-run 不碰盘。 */
	static bool ParseDocument(const FString& InRootBody, FForgeDocument& OutDocument)
	{
		const FString Text = FString::Printf(TEXT(R"({
			"kind": "texture",
			"schemaVersion": 1,
			"revision": 1,
			"asset": "/Game/Test/Texture/ForgeProbe",
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

	static FString MakeRoot(
		const FString& InClass,
		const FString& InSource,
		const FString& InProps = TEXT("{}"),
		const FString& InChildren = FString())
	{
		const FString SourceField = InSource.IsEmpty()
			? FString()
			: FString::Printf(TEXT(", \"source\": \"%s\""), *InSource);
		const FString ChildrenField = InChildren.IsEmpty()
			? FString()
			: FString::Printf(TEXT(", \"children\": %s"), *InChildren);

		return FString::Printf(TEXT(R"({
			"class": "%s",
			"name": "ForgeProbe",
			"props": %s%s%s
		})"), *InClass, *InProps, *SourceField, *ChildrenField);
	}

	TEST_METHOD(纹理资产被分派到_texture)
	{
		// 这条接线断了，资产右键菜单的"导入/导出配置"与导出分派都会失灵。
		ASSERT_THAT(AreEqual(FString(TextureForge::Kind), ForgeKindRegistry::ClassifyAssetClass(UTexture2D::StaticClass()),
			TEXT("UTexture2D 必须被分派到 texture")));
	}

	TEST_METHOD(写子节点被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("Texture2D"),
			ProbeSource(),
			TEXT("{}"),
			TEXT(R"([ { "class": "Texture2D", "name": "Child" } ])")), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(TextureForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("纹理没有子节点，写了 children 必须拒绝")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.children"), EForgeSeverity::Error),
			TEXT("诊断应指向 children")));
	}

	TEST_METHOD(class_必须是_UTexture2D_家族)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("/Script/Engine.Material"),
			ProbeSource()), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(TextureForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("别的资产类不能被当成纹理")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.class"), EForgeSeverity::Error),
			TEXT("诊断应指向 root.class")));
	}

	TEST_METHOD(新纹理必须写_source)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("Texture2D"),
			TEXT("")), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(TextureForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("目标不存在且没写 source：内容无从谈起，必须拒绝")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.source"), EForgeSeverity::Error),
			TEXT("诊断应指向 root.source")));
	}

	TEST_METHOD(不存在的源图被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("Texture2D"),
			TEXT("SourceArt/NoSuchLogo.png")), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(TextureForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("源图不存在必须报错，不能静默跳过")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.source"), EForgeSeverity::Error),
			TEXT("诊断应指向 root.source")));
	}

	TEST_METHOD(属性名写错被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("Texture2D"),
			ProbeSource(),
			TEXT(R"({ "NoSuchSetting": 1 })")), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(TextureForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("写错的属性名不能静默通过")));
		ASSERT_THAT(IsTrue(Report.Diagnostics.Num() > 0, TEXT("至少给出一条诊断")));
	}

	TEST_METHOD(目标不存在时计划是创建)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("Texture2D"),
			ProbeSource(),
			TEXT(R"({ "SRGB": false, "LODGroup": "TEXTUREGROUP_ColorLookupTable" })")), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsTrue(TextureForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("目标不存在 + 合法源图时应给出创建计划")));
		ASSERT_THAT(AreEqual(1, Report.Created, TEXT("计划里应有一个创建动作")));
		ASSERT_THAT(IsTrue(!Report.Actions.IsEmpty() && Report.Actions[0].Action == EForgeAction::Create,
			TEXT("那一个动作就是 Create")));
	}

	TEST_METHOD(关键参数总是出现在导出里)
	{
		FORGE_SKIP_WITHOUT_ASSET(ForgeTestAssets::Texture);
		// 关键参数（压缩/寻址/mip……）哪怕等于默认也必须显式出现——配置读者最主要的抓手。
		// 用项目里真实的 LUT 走完整导出；这条守着"导出方向永远写全关键参数"的约定。
		FForgeDocument Document;
		FForgeReport Report;
		ASSERT_THAT(IsTrue(ForgePipeline::Export(
			ForgeTestAssets::Texture, Document, Report),
			TEXT("导出真实 LUT 应成功")));

		ASSERT_THAT(IsTrue(Document.Root.Props.IsValid(), TEXT("导出结果应有 props")));
		if (!Document.Root.Props.IsValid())
		{
			return;
		}

		const TCHAR* const Keys[] = {
			TEXT("SRGB"), TEXT("CompressionSettings"), TEXT("LODGroup"), TEXT("MipGenSettings"),
			TEXT("MaxTextureSize"), TEXT("Filter"), TEXT("AddressX"), TEXT("AddressY"),
			TEXT("NeverStream"), TEXT("CompressionNoAlpha"),
		};
		for (const TCHAR* Key : Keys)
		{
			ASSERT_THAT(IsTrue(Document.Root.Props->HasField(Key),
				FString::Printf(TEXT("关键参数 %s 必须显式出现在导出的 props 里"), Key)));
		}
	}
};

#endif // WITH_AUTOMATION_WORKER
