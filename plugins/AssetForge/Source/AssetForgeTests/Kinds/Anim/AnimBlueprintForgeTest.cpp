// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"
#include "ForgeTestAssets.h"

#include "Kinds/Anim/AnimBlueprintForge.h"
#include "Registry/ForgeKindRegistry.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgePipeline.h"
#include "Core/ForgeTypes.h"

#if WITH_AUTOMATION_WORKER

/**
 * 动画蓝图处理器（anim-blueprint）的守门用例。
 *
 * 测的是「这个 kind 才知道、通用层给不了」的判定：
 *
 * - AnimGraph 占位根下只认状态机，且至少要有一个；
 * - 状态机里只认状态、至少要有一个状态、状态名不能重复；
 * - 入口状态必须真的在状态列表里，转移的两个端点同理——
 *   这些引用错一个，行为就是"看起来配好了但永远不走那根转移"。
 *
 * 走 dry-run 与纯函数：不建包、不写盘；`skeleton` 用真实存在的骨架路径
 * （dry-run 只读不写），其余引用都用不存在的目标——见 skill `tk-anim-config`。
 */
TEST_CLASS_WITH_FLAGS(AnimBlueprintForgeTest, "AssetForge.AnimBlueprint",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 造一份最小文档，只换 root 一段；asset 与 skeleton 都是真实路径的读、不写盘。 */
	static bool ParseDocument(const FString& InRootBody, FForgeDocument& OutDocument)
	{
		const FString Text = FString::Printf(TEXT(R"({
			"kind": "anim-blueprint",
			"schemaVersion": 1,
			"revision": 1,
			"asset": "/Game/Test/Anim/ABP_ForgeProbe",
			"parent": "/Script/Engine.AnimInstance",
			"root": %s,
			"skeleton": "/Game/AssetForgeTest/SKEL_ForgeTest.SKEL_ForgeTest"
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

	/** AnimGraph 占位根，children 是状态机列表。 */
	static FString MakeGraph(const FString& InChildren)
	{
		return FString::Printf(TEXT(R"({ "class": "AnimGraph", "name": "AnimGraph", "children": %s })"), *InChildren);
	}

	/** 一个状态机；children 是状态列表、machinery 追加 entry / transitions 字段。 */
	static FString MakeStateMachine(
		const FString& InName, const FString& InChildren, const FString& InMachinery = FString())
	{
		return FString::Printf(TEXT(R"({ "class": "StateMachine", "name": "%s"%s, "children": %s })"),
			*InName, *InMachinery, *InChildren);
	}

	static FString MakeState(const FString& InName)
	{
		return FString::Printf(TEXT(R"({ "class": "State", "name": "%s" })"), *InName);
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
		ASSERT_THAT(IsTrue(ForgeKindRegistry::GetSupportedKinds().Contains(AnimBlueprintForge::Kind),
			TEXT("anim-blueprint 必须出现在支持的 kind 名单里（窗口与校验都读它）")));
	}

	TEST_METHOD(根节点不是_AnimGraph_被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			FString::Printf(TEXT(R"({ "class": "StateMachine", "name": "Locomotion", "children": [ %s ] })"),
				*MakeState(TEXT("Idle"))),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(AnimBlueprintForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("根节点必须是 AnimGraph 占位")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.class"), EForgeSeverity::Error),
			TEXT("诊断应指向 root.class")));
	}

	TEST_METHOD(没有状态机被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeGraph(TEXT("[]")), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(AnimBlueprintForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("空 AnimGraph 没有可做的事，必须拒绝")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.children"), EForgeSeverity::Error),
			TEXT("诊断应指向 root.children")));
	}

	TEST_METHOD(状态机下写了非状态被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			MakeGraph(FString::Printf(TEXT("[ %s ]"),
				*MakeStateMachine(TEXT("Locomotion"), FString::Printf(TEXT("[ %s ]"),
					*MakeStateMachine(TEXT("Nested"), FString::Printf(TEXT("[ %s ]"), *MakeState(TEXT("Idle")))))))),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(AnimBlueprintForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("状态机下只认状态")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.children[0].children[0].class"), EForgeSeverity::Error),
			TEXT("诊断应指向该节点（状态机与状态都用 children[索引] 定位）")));
	}

	TEST_METHOD(状态重名被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			MakeGraph(FString::Printf(TEXT("[ %s ]"),
				*MakeStateMachine(TEXT("Locomotion"), FString::Printf(TEXT("[ %s, %s ]"),
					*MakeState(TEXT("Idle")), *MakeState(TEXT("Idle")))))),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(AnimBlueprintForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("状态名是匹配键，重名必须拒绝")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.children[0].children[1].name"), EForgeSeverity::Error),
			TEXT("诊断应指向重复的那个状态")));
	}

	TEST_METHOD(入口状态不在列表里被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			MakeGraph(FString::Printf(TEXT("[ %s ]"),
				*MakeStateMachine(TEXT("Locomotion"), FString::Printf(TEXT("[ %s ]"), *MakeState(TEXT("Idle"))),
					TEXT(R"(, "entry": "Nope")")))),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(AnimBlueprintForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("入口状态不存在，行为就是「配了但不生效」，必须拒绝")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.children[0].extra.entry"), EForgeSeverity::Error),
			TEXT("诊断应指向该状态机的 entry")));
	}

	TEST_METHOD(转移指向不存在的状态被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			MakeGraph(FString::Printf(TEXT("[ %s ]"),
				*MakeStateMachine(TEXT("Locomotion"), FString::Printf(TEXT("[ %s ]"), *MakeState(TEXT("Idle"))),
					TEXT(R"(, "transitions": [ { "from": "Idle", "to": "Nope" } ])")))),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(AnimBlueprintForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("转移的端点必须真实存在，否则那根转移永远不走")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.children[0].extra.transitions[0]"), EForgeSeverity::Error),
			TEXT("诊断应指向该转移")));
	}

	TEST_METHOD(合法状态机在_dry_run_下给出计划)
	{
		FORGE_SKIP_WITHOUT_ASSET(ForgeTestAssets::Skeleton);
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			MakeGraph(FString::Printf(TEXT("[ %s ]"),
				*MakeStateMachine(TEXT("Locomotion"),
					FString::Printf(TEXT("[ %s, %s ]"), *MakeState(TEXT("Idle")), *MakeState(TEXT("Forward"))),
					TEXT(R"(, "entry": "Idle", "transitions": [ { "from": "Idle", "to": "Forward" } ])")))),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsTrue(AnimBlueprintForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("合法状态机应给出计划，而不是报错")));
		ASSERT_THAT(IsTrue(!Report.Actions.IsEmpty(),
			TEXT("目标资产不存在 → 计划里应有创建动作")));
	}
};

#endif // WITH_AUTOMATION_WORKER
