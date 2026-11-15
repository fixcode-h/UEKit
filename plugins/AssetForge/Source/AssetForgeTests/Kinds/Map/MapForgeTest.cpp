// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Registry/ForgeKindRegistry.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgePipeline.h"
#include "Core/ForgeTypes.h"
#include "Kinds/Map/MapForge.h"

#include "Engine/World.h"

#if WITH_AUTOMATION_WORKER

/**
 * 地图处理器的守门用例。
 *
 * 测的是「这个 kind 才知道、通用层给不了」的判定：
 *
 * - 地图配置只描述世界设置：写了 `children` 必须在计划阶段拦下，而不是悄悄忽略；
 * - `root.class` 必须是 `AWorldSettings` 家族：写别的类要被明确指出来；
 * - 目标不存在时的计划是**创建**（空地图由导入创建），而不是报错。
 *
 * 走 dry-run 与纯函数：不建包、不写盘，用例不依赖任何已有资产。
 * 「创建出来的地图能不能用」由端到端回环负责，这里覆盖不了。
 */
TEST_CLASS_WITH_FLAGS(MapForgeTest, "AssetForge.Map",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 造一份最小文档，只换 root 那一段；asset 指向一个必定不存在的路径——dry-run 不碰盘。 */
	static bool ParseDocument(const FString& InRootBody, FForgeDocument& OutDocument)
	{
		const FString Text = FString::Printf(TEXT(R"({
			"kind": "map",
			"schemaVersion": 1,
			"revision": 1,
			"asset": "/Game/Test/Maps/ForgeProbe",
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

	static FString MakeRoot(const FString& InClass, const FString& InProps, const FString& InChildren = FString())
	{
		const FString ChildrenField = InChildren.IsEmpty()
			? FString()
			: FString::Printf(TEXT(", \"children\": %s"), *InChildren);

		return FString::Printf(TEXT(R"({
			"class": "%s",
			"name": "WorldSettings",
			"props": %s%s
		})"), *InClass, *InProps, *ChildrenField);
	}

	TEST_METHOD(地图资产被分派到_map)
	{
		// 地图（.umap）的资产类就是 UWorld——这条接线断了，右键菜单与导出分派都会失灵。
		ASSERT_THAT(AreEqual(FString(MapForge::Kind), ForgeKindRegistry::ClassifyAssetClass(UWorld::StaticClass()),
			TEXT("UWorld 必须被分派到 map")));
	}

	TEST_METHOD(写子节点被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("/Script/Engine.WorldSettings"),
			TEXT("{}"),
			TEXT(R"([ { "class": "Actor", "name": "Floor" } ])")), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(MapForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("地图内容不归本处理器表达，写了 children 必须拒绝")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.children"), EForgeSeverity::Error),
			TEXT("诊断应指向 children")));
	}

	TEST_METHOD(世界设置类必须属于_AWorldSettings_家族)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("/Script/Engine.DataTable"),
			TEXT("{}")), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(MapForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("别的资产类不能被当成世界设置")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.class"), EForgeSeverity::Error),
			TEXT("诊断应指向 root.class")));
	}

	TEST_METHOD(不存在的世界设置类被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("/Script/Engine.NoSuchWorldSettings"),
			TEXT("{}")), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(MapForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("找不到的类必须报错，不能猜")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.class"), EForgeSeverity::Error),
			TEXT("诊断应指向 root.class")));
	}

	TEST_METHOD(属性名写错被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("/Script/Engine.WorldSettings"),
			TEXT(R"({ "NoSuchSetting": 1 })")), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(MapForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("写错的属性名不能静默通过")));
		ASSERT_THAT(IsTrue(Report.Diagnostics.Num() > 0, TEXT("至少给出一条诊断")));
	}

	TEST_METHOD(目标不存在时计划是创建)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("/Script/Engine.WorldSettings"),
			TEXT(R"({ "KillZ": -5000 })")), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsTrue(MapForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("目标不存在时应给出创建计划（空地图由导入创建），而不是报错")));
		ASSERT_THAT(AreEqual(1, Report.Created, TEXT("计划里应有一个创建动作")));
		ASSERT_THAT(IsTrue(!Report.Actions.IsEmpty() && Report.Actions[0].Action == EForgeAction::Create,
			TEXT("那一个动作就是 Create")));
	}
};

#endif // WITH_AUTOMATION_WORKER
