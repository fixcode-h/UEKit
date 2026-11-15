// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"
#include "ForgeTestAssets.h"

#include "Kinds/Anim/AnimSequenceForge.h"
#include "Registry/ForgeKindRegistry.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgePipeline.h"
#include "Core/ForgeTypes.h"

#if WITH_AUTOMATION_WORKER

/**
 * 动画资产（纳管型 kind）的守门用例。
 *
 * 测的是「纳管」这个定位自己才知道的判定：
 * - 资产**必须已存在**（找不到就报错，绝不凭空生成——这是与生成型 kind 的分界）；
 * - 字段名写错要在**动资产之前**被拦下；
 * - 对真实资产的 dry-run 要算得出计划（只读，不写盘）。
 *
 * 幂等（导出 → 导入不写盘）由端到端自检配置负责，测试不碰真实资产的内容。
 */
TEST_CLASS_WITH_FLAGS(AnimSequenceForgeTest, "AssetForge.AnimSequence",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	static bool ParseDocument(const FString& InAssetPath, const FString& InProps, FForgeDocument& OutDocument)
	{
		const FString Text = FString::Printf(TEXT(R"({
			"kind": "anim-sequence",
			"schemaVersion": 1,
			"revision": 1,
			"asset": "%s",
			"root":
			{
				"class": "AnimSequence",
				"name": "Probe",
				"props": %s
			}
		})"), *InAssetPath, *InProps);

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

	TEST_METHOD(类型已登记在管道里)
	{
		ASSERT_THAT(IsTrue(ForgeKindRegistry::GetSupportedKinds().Contains(AnimSequenceForge::Kind),
			TEXT("anim-sequence 必须出现在支持的 kind 名单里")));
	}

	TEST_METHOD(资产不存在时报错而不是生成)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(TEXT("/Game/No/Such/Anim_Probe"), TEXT("{}"), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(AnimSequenceForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("纳管型 kind 找不到资产必须报错（不凭空生成）")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("asset"), EForgeSeverity::Error),
			TEXT("诊断应指向 asset")));
	}

	TEST_METHOD(字段名写错被拒绝)
	{
		FORGE_SKIP_WITHOUT_ASSET(ForgeTestAssets::AnimSequence);
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			ForgeTestAssets::AnimSequence,
			TEXT(R"({ "NoSuchProperty": 1 })"),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(AnimSequenceForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("写错的字段名不能静默通过")));
		ASSERT_THAT(IsTrue(Report.Diagnostics.Num() > 0, TEXT("至少给出一条诊断")));
	}

	TEST_METHOD(对真实动画的_dry_run_算得出计划)
	{
		FORGE_SKIP_WITHOUT_ASSET(ForgeTestAssets::AnimSequence);
		// 只读：dry-run 不写盘，安全。
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			ForgeTestAssets::AnimSequence,
			TEXT(R"({ "RateScale": 1.0 })"),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsTrue(AnimSequenceForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("对真实动画的 dry-run 应成功返回计划")));
	}
};

#endif // WITH_AUTOMATION_WORKER
